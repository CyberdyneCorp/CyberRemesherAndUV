#include "cyber/core/pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>

#include "cyber/core/bvh.hpp"
#include "cyber/core/detail/parallel_chunks.hpp"
#include "cyber/core/isotropic.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/core/reference_surface.hpp"
#include "cyber/core/threading.hpp"

namespace cyber::remesh {

namespace {

double totalSurfaceArea(const Mesh& mesh) {
    double area = 0.0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);
        for (std::size_t i = 2; i < verts.size(); ++i) {
            const Vec3 a = mesh.position(verts[0]);
            const Vec3 b = mesh.position(verts[i - 1]);
            const Vec3 c = mesh.position(verts[i]);
            area += 0.5 * static_cast<double>(length(cross(b - a, c - a)));
        }
    }
    return area;
}

// Largest |coordinate| anywhere on the mesh. float spacing at magnitude m is
// m * FLT_EPSILON, so this is what decides whether a derived edge length is
// representable at all.
float maxAbsCoordinate(const Mesh& mesh) {
    float maxAbs = 0.0f;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const Vec3 p = mesh.position(v);
        maxAbs = std::max({maxAbs, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
    }
    return maxAbs;
}

// The isotropic split pass refuses any split whose halves the float grid cannot
// separate (isotropic.cpp, kMinResolvableSpacings), so a target edge length
// below that floor cannot be honoured: the geometry is already quantized onto
// a grid coarser than the request. That happens for a mesh parked far from the
// world origin — a scan in UTM/site coordinates, a glTF scene with a baked
// translation — and it used to be silent, the run simply coming back at
// roughly the input density.
//
// Clamp-and-warn, the same contract validate() applies to every other
// out-of-range parameter, and for a concrete reason beyond honesty: the
// downstream quadrangulator does NOT share the float grid (the vendored
// seamless solve works in double), so it accepted the un-honourable target and
// tried to fit thousands of quads onto the handful of distinct positions the
// coordinates can express. On a unit sphere at 1e7 that turned a 0.02 s run
// into one still going after five minutes. Clamping asks every stage for the
// density the coordinates can actually carry.
constexpr float kMinResolvableSpacings = 4.0f;  // mirrors isotropic.cpp

struct ResolvedEdgeLength {
    float edgeLength = 0.0f;
    std::optional<ParameterIssue> issue;
};

ResolvedEdgeLength resolveAgainstCoordinates(const Mesh& mesh, float targetEdgeLength) {
    const float maxAbs = maxAbsCoordinate(mesh);
    const float resolution =
        kMinResolvableSpacings * maxAbs * std::numeric_limits<float>::epsilon();
    // `resolution > 0` and `isfinite` also reject a mesh whose coordinates are
    // NaN or infinite: there is no representable density to clamp to there, and
    // substituting one would replace a coarse result with an unusable one.
    if (!(targetEdgeLength <= resolution) || !(resolution > 0.0f) || !std::isfinite(resolution)) {
        return {targetEdgeLength, std::nullopt};
    }
    char message[320];
    std::snprintf(message, sizeof(message),
                  "target edge length %.6g is below the %.6g float resolution of coordinates up "
                  "to %.6g: the mesh is too far from the origin for the requested density. Using "
                  "%.6g instead — move the mesh near the origin to get the density you asked for.",
                  static_cast<double>(targetEdgeLength), static_cast<double>(resolution),
                  static_cast<double>(maxAbs), static_cast<double>(resolution));
    return {resolution, ParameterIssue{"targetQuadCount", message, false}};
}

// Extracts one island into a standalone mesh (local vertex indexing).
Mesh extractIsland(const Mesh& source, const std::vector<FaceId>& faces) {
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faceLists;
    std::vector<Index> remap(source.vertexCapacity(), kInvalidIndex);
    for (const FaceId f : faces) {
        std::vector<Index> face;
        for (const VertexId v : source.faceVertices(f)) {
            if (remap[v.value] == kInvalidIndex) {
                remap[v.value] = static_cast<Index>(positions.size());
                positions.push_back(source.position(v));
            }
            face.push_back(remap[v.value]);
        }
        faceLists.push_back(std::move(face));
    }
    return Mesh::fromIndexed(positions, faceLists);
}

// Weld coincident vertices so a polygon soup becomes a connected surface. Some inputs
// (e.g. an exported cube whose six faces don't share vertices) arrive as disconnected
// patches meeting at coincident-but-distinct vertices; islands() then sees N patches,
// quadrangulates each independently, and their shared-edge boundaries disagree at high
// density — leaving open seams (hundreds of boundary edges). Merging duplicate positions
// makes the surface one component before quadrangulation, so there is nothing to stitch.
// Returns the input unchanged when nothing is coincident (already-welded meshes stay
// bit-identical, preserving determinism on the corpus).
Mesh weldCoincidentVertices(const Mesh& mesh) {
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    if (pos.empty()) {
        return mesh;
    }
    Vec3 lo = pos[0];
    Vec3 hi = pos[0];
    for (const Vec3& p : pos) {
        lo = min(lo, p);
        hi = max(hi, p);
    }
    const float eps = std::max(length(hi - lo) * 1e-6f, 1e-9f);
    const float inv = 1.0f / eps;
    const auto key = [&](const Vec3& p) {
        return std::array<long long, 3>{std::llround(static_cast<double>(p.x * inv)),
                                        std::llround(static_cast<double>(p.y * inv)),
                                        std::llround(static_cast<double>(p.z * inv))};
    };
    std::map<std::array<long long, 3>, Index> lookup;
    std::vector<Vec3> welded;
    std::vector<Index> remap(pos.size());
    for (std::size_t i = 0; i < pos.size(); ++i) {
        const auto [it, inserted] =
            lookup.try_emplace(key(pos[i]), static_cast<Index>(welded.size()));
        if (inserted) {
            welded.push_back(pos[i]);
        }
        remap[i] = it->second;
    }
    if (welded.size() == pos.size()) {
        return mesh;  // nothing coincident
    }
    std::vector<std::vector<Index>> weldedFaces;
    weldedFaces.reserve(faces.size());
    for (const auto& f : faces) {
        std::vector<Index> nf;
        for (const Index v : f) {
            const Index w = remap[v];
            if (nf.empty() || nf.back() != w) {
                nf.push_back(w);  // drop consecutive duplicates a weld can create
            }
        }
        while (nf.size() > 1 && nf.front() == nf.back()) {
            nf.pop_back();
        }
        if (nf.size() >= 3) {  // skip faces that collapsed to a degenerate
            weldedFaces.push_back(std::move(nf));
        }
    }
    return Mesh::fromIndexed(welded, weldedFaces);
}

// Make every face's winding agree with its neighbours (flood-fill orientation
// repair). Inconsistent winding — common in AI-generated, scanned, and CAD-export
// meshes — leaves the field/extractor with contradictory normals and yields a
// non-manifold quad output. BFS over faces across MANIFOLD shared edges (exactly two
// faces): two consistently-oriented faces traverse a shared edge in OPPOSITE
// directions, so a neighbour that traverses it the SAME way is flipped. Each closed
// component is then flipped as a whole to outward (positive signed volume). On
// already-consistent, outward input (the corpus) nothing flips — the result is
// bit-identical, preserving determinism.
Mesh orientFacesConsistently(const Mesh& mesh) {
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    if (faces.empty()) {
        return mesh;
    }
    // Undirected edge -> list of (face, loopIsMinToMax): whether the face's loop
    // traverses the edge from min(a,b) to max(a,b).
    std::map<std::pair<Index, Index>, std::vector<std::pair<int, bool>>> edgeMap;
    for (int fi = 0; fi < static_cast<int>(faces.size()); ++fi) {
        const auto& f = faces[static_cast<std::size_t>(fi)];
        const std::size_t n = f.size();
        for (std::size_t k = 0; k < n; ++k) {
            const Index a = f[k], b = f[(k + 1) % n];
            edgeMap[{std::min(a, b), std::max(a, b)}].push_back({fi, a < b});
        }
    }
    std::vector<char> visited(faces.size(), 0);
    std::vector<char> flip(faces.size(), 0);  // effective direction = loopIsMinToMax XOR flip
    for (int s = 0; s < static_cast<int>(faces.size()); ++s) {
        if (visited[static_cast<std::size_t>(s)]) {
            continue;
        }
        std::vector<int> comp;
        std::queue<int> q;
        q.push(s);
        visited[static_cast<std::size_t>(s)] = 1;
        bool closedComponent = true;  // no boundary/non-manifold edges seen
        while (!q.empty()) {
            const int fi = q.front();
            q.pop();
            comp.push_back(fi);
            const auto& f = faces[static_cast<std::size_t>(fi)];
            const std::size_t n = f.size();
            for (std::size_t k = 0; k < n; ++k) {
                const Index a = f[k], b = f[(k + 1) % n];
                const auto key = std::make_pair(std::min(a, b), std::max(a, b));
                const auto& incident = edgeMap[key];
                if (incident.size() != 2) {
                    closedComponent = false;  // open patch: signed volume is meaningless
                    continue;  // boundary or non-manifold edge: don't propagate across
                }
                for (const auto& [nf, nfwd] : incident) {
                    if (nf == fi || visited[static_cast<std::size_t>(nf)]) {
                        continue;
                    }
                    const bool effI = (a < b) != (flip[static_cast<std::size_t>(fi)] != 0);
                    // neighbour must end up with the opposite effective direction.
                    const bool wantJ = !effI;
                    flip[static_cast<std::size_t>(nf)] = (nfwd != wantJ) ? 1 : 0;
                    visited[static_cast<std::size_t>(nf)] = 1;
                    q.push(nf);
                }
            }
        }
        // Orient the whole component outward: flip all if its signed volume is negative.
        // Only meaningful for a CLOSED component — an open patch's signed volume is
        // arbitrary, so leave it as the flood-fill made it (consistent, input-preserving).
        if (!closedComponent) {
            continue;
        }
        double vol = 0.0;
        for (const int fi : comp) {
            const auto& f = faces[static_cast<std::size_t>(fi)];
            const bool fl = flip[static_cast<std::size_t>(fi)] != 0;
            for (std::size_t k = 1; k + 1 < f.size(); ++k) {
                const Vec3 p0 = pos[f[0]];
                const Vec3 p1 = pos[fl ? f[f.size() - k] : f[k]];
                const Vec3 p2 = pos[fl ? f[f.size() - k - 1] : f[k + 1]];
                vol += static_cast<double>(dot(p0, cross(p1, p2))) / 6.0;
            }
        }
        if (vol < 0.0) {
            for (const int fi : comp) {
                flip[static_cast<std::size_t>(fi)] ^= 1;
            }
        }
    }
    std::size_t flipped = 0;
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        if (flip[fi]) {
            std::reverse(faces[fi].begin(), faces[fi].end());
            ++flipped;
        }
    }
    if (flipped == 0) {
        return mesh;  // already consistent + outward: bit-identical passthrough
    }
    return Mesh::fromIndexed(pos, faces);
}

// Run fn over the vertex index range [0,n) split into as many chunks as the host
// allows workers (hardware concurrency unless cyber::setMaxWorkerThreads capped it).
// The relax loops are per-vertex independent, so the split is byte-identical to the
// serial loop; each thread does enough work (thousands of BVH projections) that the
// spawn/join overhead is negligible (unlike the tiny per-CG-iter spmv).
//
// A throw from fn (a relax pass that runs out of memory, say) comes back on the
// calling thread rather than terminating the process on a worker, so the C ABI
// still turns it into a CyberStatus.
template <typename Fn>
void parallelVertexRange(std::size_t n, const Fn& fn) {
    cyber::detail::forEachChunk(0, n, cyber::workerThreadsFor(n), fn);
}

// Tangential Laplacian step for an interior vertex: move toward the 1-ring
// centroid, keeping only the component in the vertex's tangent plane so the
// vertex slides across the surface without denting it inward.
Vec3 vertexNormal(const Mesh& mesh, VertexId v) {
    Vec3 normal{};
    for (const FaceId f : mesh.vertexFaces(v)) {
        normal += mesh.faceNormal(f);
    }
    return normalized(normal);
}

// An orthonormal tangent basis (u, w) of the plane normal to n.
void tangentBasis(Vec3 n, Vec3& u, Vec3& w) {
    if (std::fabs(n.x) > std::fabs(n.y)) {
        const float il = 1.0f / std::sqrt(n.x * n.x + n.z * n.z);
        u = Vec3{-n.z * il, 0.0f, n.x * il};
    } else {
        const float il = 1.0f / std::sqrt(n.y * n.y + n.z * n.z);
        u = Vec3{0.0f, n.z * il, -n.y * il};
    }
    w = cross(n, u);
}

// Best-fit-square ("shape-matching") target for a vertex: each incident quad is
// fitted to the square (same centroid, same average radius, best rotation in the
// face's tangent plane) that its four corners are closest to, and the vertex
// moves toward the average of its corner position across those ideal squares.
// A square has 90-degree corners AND equal edges, so this pushes both angle and
// edge-length regularity at once — the geometric equivalent of a globally
// consistent integer grid, without a global solve. Interior only; the result is
// projected back onto the surface by the caller.
Vec3 shapeMatchTarget(const Mesh& mesh, VertexId v, float lambda, float targetRadius) {
    const Vec3 p = mesh.position(v);
    Vec3 accum{};
    int count = 0;
    for (const FaceId f : mesh.vertexFaces(v)) {
        const auto verts = mesh.faceVertices(f);
        if (verts.size() != 4) {
            continue;  // shape-matching is defined for quads
        }
        Vec3 m{};
        for (const VertexId vv : verts) {
            m += mesh.position(vv);
        }
        m = m / 4.0f;
        Vec3 u, w;
        tangentBasis(mesh.faceNormal(f), u, w);

        // Project corners to the face's 2D tangent frame. The square's radius is
        // the shared global target (uniform sizing => every quad the same square,
        // giving both 90-degree corners AND equal edge lengths across the mesh),
        // falling back to the quad's own average radius when no target is given.
        std::array<float, 4> ax{}, ay{};
        float radius = 0.0f;
        for (std::size_t i = 0; i < 4; ++i) {
            const Vec3 d = mesh.position(verts[i]) - m;
            ax[i] = dot(d, u);
            ay[i] = dot(d, w);
            radius += std::sqrt(ax[i] * ax[i] + ay[i] * ay[i]);
        }
        radius = targetRadius > 0.0f ? targetRadius : radius * 0.25f;

        // Canonical square corners (45,135,225,315 deg) in the same winding as
        // the face; solve the 2D Procrustes rotation aligning them to the actual
        // corners: theta = atan2(sum canon x actual, sum canon . actual).
        constexpr float kQuarterPi = 0.78539816339744830961f;
        std::array<float, 4> cx{}, cy{};
        float sumCross = 0.0f, sumDot = 0.0f;
        for (std::size_t i = 0; i < 4; ++i) {
            const float phi = kQuarterPi + static_cast<float>(i) * (2.0f * kQuarterPi);
            cx[i] = radius * std::cos(phi);
            cy[i] = radius * std::sin(phi);
            sumCross += cx[i] * ay[i] - cy[i] * ax[i];
            sumDot += cx[i] * ax[i] + cy[i] * ay[i];
        }
        const float theta = std::atan2(sumCross, sumDot);
        const float ct = std::cos(theta), st = std::sin(theta);

        // This vertex's index within the quad -> its ideal (rotated) corner.
        for (std::size_t i = 0; i < 4; ++i) {
            if (verts[i] == v) {
                const float tx = ct * cx[i] - st * cy[i];
                const float ty = st * cx[i] + ct * cy[i];
                accum += m + u * tx + w * ty;
                ++count;
                break;
            }
        }
    }
    if (count == 0) {
        return p;
    }
    const Vec3 target = accum / static_cast<float>(count);
    const Vec3 normal = vertexNormal(mesh, v);
    Vec3 delta = (target - p) * lambda;
    delta = delta - normal * dot(delta, normal);  // stay tangential; caller reprojects
    return p + delta;
}

Vec3 tangentialTarget(const Mesh& mesh, VertexId v, float lambda) {
    const auto edges = mesh.vertexEdges(v);
    Vec3 centroid{};
    for (const EdgeId e : edges) {
        const auto [a, b] = mesh.edgeVertices(e);
        centroid += mesh.position(a == v ? b : a);
    }
    centroid = centroid / static_cast<float>(edges.size());
    const Vec3 normal = vertexNormal(mesh, v);
    const Vec3 p = mesh.position(v);
    Vec3 delta = (centroid - p) * lambda;
    delta = delta - normal * dot(delta, normal);  // keep only the tangential slide
    return p + delta;
}

// Relaxes a quad mesh, re-projecting onto the source surface every iteration.
// Linear subdivision followed by closest-point snapping leaves many degenerate
// quads — adjacent vertices land nearly on top of one another (near-zero edges,
// ~0-degree corners), so a pure-quad result is clean topologically but riddled
// with slivers. Interior vertices slide toward their 1-ring centroid to
// equalize quad shape while the per-iteration projection keeps them on the
// source surface, so the silhouette is preserved while element quality
// improves. Feature (sharp crease) and boundary vertices are frozen so creases
// and open borders keep their subdivided positions — sliding them along the
// faceted crease instead reprojects erratically and creates new slivers.
void relaxQuadMesh(Mesh& mesh, const ReferenceSurface& reference, float sharpEdgeDegrees,
                   int iterations, float lambda, bool shapeMatch = false,
                   const std::vector<std::array<Vec3, 2>>& creaseNetwork = {},
                   const GuidanceField* density = nullptr) {
    mesh.tagFeatureEdges(sharpEdgeDegrees);
    std::vector<bool> constrained(mesh.vertexCapacity(), false);
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || !(mesh.isFeatureEdge(e) || mesh.isBoundaryEdge(e))) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        constrained[a.value] = true;
        constrained[b.value] = true;
    }
    // Uniform target square half-diagonal for shape matching: the current mean
    // quad edge / sqrt(2). Recomputed each sweep so it tracks the mesh as it
    // uniformizes and converges to a consistent global cell size. Also sets the
    // "sits ON the crease" tolerance below (5% of a cell: crease vertices land at
    // ~float-epsilon of the polyline, everything else at least half a cell away).
    const auto meanQuadEdge = [&]() {
        double sum = 0.0;
        std::size_t n = 0;
        for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
            const FaceId f{fi};
            if (!mesh.isAlive(f) || mesh.faceSize(f) != 4) {
                continue;
            }
            const auto verts = mesh.faceVertices(f);
            for (std::size_t k = 0; k < 4; ++k) {
                sum += static_cast<double>(
                    length(mesh.position(verts[k]) - mesh.position(verts[(k + 1) % 4])));
                ++n;
            }
        }
        return n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
    };

    // Curved creases defeat the dihedral tag above: a coarse quad on a curved wall is
    // bent, its Newell normal tilts toward the wall's centre, and the crease dihedral
    // reads well under the threshold (a quarter-density cylinder rim reads ~83 degrees
    // against the 90-degree tag) — so rim vertices were never frozen and the relax
    // smoothed the sharp feature away (cylinder --pure-quads recall 1.00 -> 0.75).
    // Freeze any vertex sitting ON the source crease polyline network instead; the
    // network is empty for feature-free meshes (behavior unchanged).
    const float creaseTol = 0.05f * meanQuadEdge();
    if (!creaseNetwork.empty() && creaseTol > 0.0f) {
        for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
            const VertexId v{vi};
            if (!mesh.isAlive(v) || constrained[vi]) {
                continue;
            }
            const Vec3 p = mesh.position(v);
            float best = std::numeric_limits<float>::max();
            for (const auto& seg : creaseNetwork) {
                const Vec3 ab = seg[1] - seg[0];
                const float len2 = dot(ab, ab);
                const float t =
                    len2 > 1e-20f ? std::clamp(dot(p - seg[0], ab) / len2, 0.0f, 1.0f) : 0.0f;
                const Vec3 q = seg[0] + ab * t;
                best = std::min(best, dot(p - q, p - q));
                if (best <= 0.0f) {
                    break;
                }
            }
            constrained[vi] = best < creaseTol * creaseTol;
        }
    }

    std::vector<Vec3> newPos(mesh.vertexCapacity());
    std::vector<bool> move(mesh.vertexCapacity(), false);
    // Both per-vertex loops are independent (compute reads unchanged positions and writes
    // its own newPos slot; project reads newPos and writes its own position slot), and the
    // reference BVH is read-only — so running them across threads is byte-IDENTICAL (no
    // reduction, per-vertex float ops). The dominant cost is the reference.project() BVH
    // query per vertex per iteration; on a large source mesh this relax was ~half the whole
    // remesh (8.3s on a 554k-tri source), embarrassingly parallel.
    for (int it = 0; it < iterations; ++it) {
        const float targetRadius = shapeMatch ? meanQuadEdge() * 0.70710678f : 0.0f;
        std::fill(move.begin(), move.end(), false);
        // Compute pass stays serial: the target functions read the mesh's lazily-built
        // topology/normal caches, which are not safe to populate concurrently. It is the
        // cheap pass (no BVH).
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v) || constrained[i] || mesh.vertexEdges(v).empty()) {
                continue;
            }
            // Painted density shrinks the ideal square locally, so the
            // uniform-square shape match does not re-uniformize a region the
            // sizing stage deliberately densified. Null density leaves the
            // radius exactly as it was.
            const float localRadius =
                density != nullptr ? targetRadius / std::sqrt(density->densityAt(mesh.position(v)))
                                   : targetRadius;
            newPos[i] = shapeMatch ? shapeMatchTarget(mesh, v, lambda, localRadius)
                                   : tangentialTarget(mesh, v, lambda);
            move[i] = true;
        }
        // Project pass IS parallel: reference.project() is const with a local traversal
        // stack (thread-safe), setPosition writes disjoint slots, and each vertex is
        // independent — byte-identical to the serial loop. This is the expensive pass (a
        // BVH closest-point query per vertex) and dominated the whole remesh on large meshes.
        parallelVertexRange(mesh.vertexCapacity(), [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) {
                if (!move[i]) {
                    continue;
                }
                mesh.setPosition(VertexId{static_cast<Index>(i)},
                                 reference.empty() ? newPos[i] : reference.project(newPos[i]));
            }
        });
    }
}

void countFaces(const Mesh& mesh, Statistics& stats) {
    stats.vertexCount = mesh.vertexCount();
    stats.quadCount = 0;
    stats.triangleCount = 0;
    stats.otherPolygonCount = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        const std::size_t n = mesh.faceSize(FaceId{fi});
        if (n == 4) {
            ++stats.quadCount;
        } else if (n == 3) {
            ++stats.triangleCount;
        } else {
            ++stats.otherPolygonCount;
        }
    }
}

}  // namespace

void applySmallPatchPolicy(Mesh& mesh, SmallPatchPolicy policy, int minFaces) {
    if (policy == SmallPatchPolicy::KeepAll) {
        return;
    }
    const auto patches = mesh.islands();
    if (patches.size() <= 1) {
        return;
    }
    std::size_t largest = 0;
    for (std::size_t i = 1; i < patches.size(); ++i) {
        if (patches[i].size() > patches[largest].size()) {
            largest = i;
        }
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const bool keep = policy == SmallPatchPolicy::KeepLargest
                              ? i == largest
                              : patches[i].size() >= static_cast<std::size_t>(minFaces);
        if (keep) {
            continue;
        }
        for (const FaceId f : patches[i]) {
            mesh.removeFace(f);
        }
    }
}

PipelineResult remesh(const Mesh& input, const Parameters& rawParams, ProgressSink* progress,
                      const CancelToken* cancel, const QuadrangulatorFactory& quadrangulator,
                      const QuadrangulatorFactory& fallbackQuadrangulator,
                      const Guidance* guidance) {
    PipelineResult result;

    // Stage 0: parameters (validated at every entry point — spec).
    ValidatedParameters validated = validate(rawParams);
    result.parameterIssues = validated.issues;
    // Guidance is validated on exactly the same terms: clamps are warnings that
    // reach the machine-readable report, unusable input is fatal.
    ValidatedGuidance validatedGuidance;
    if (guidance != nullptr && !guidance->empty()) {
        validatedGuidance = validateGuidance(*guidance, input.vertexCount(), input.faceCount());
        result.parameterIssues.insert(result.parameterIssues.end(),
                                      validatedGuidance.issues.begin(),
                                      validatedGuidance.issues.end());
    }
    if (!validated.ok() || !validatedGuidance.ok()) {
        result.status = RunStatus::Error;
        result.error = "invalid parameters";
        return result;
    }
    const Parameters& params = validated.params;

    if (input.faceCount() == 0) {
        result.status = RunStatus::Error;
        result.error = "input mesh is empty";
        return result;
    }

    // The guidance sampler is built from the ORIGINAL input, before triangulate /
    // weld / orient / island split, so it stays valid through every stage without
    // index remapping (it answers geometric queries, not indexed ones).
    std::unique_ptr<GuidanceField> guidanceField;
    if (!validatedGuidance.guidance.empty()) {
        guidanceField = std::make_unique<GuidanceField>(input, validatedGuidance.guidance);
        if (guidanceField->empty()) {
            guidanceField.reset();
        }
    }

    // Pure quads: remesh at quarter density, then one linear subdivision
    // turns every face into quads at the requested density (spec: pure-quad
    // option with honest residual reporting — this construction leaves zero
    // residual non-quads only after subdividing, so triangles at quarter
    // density become 3 quads).
    int effectiveQuads =
        params.pureQuads ? std::max(25, params.targetQuadCount / 4) : params.targetQuadCount;

    // Stage 1: guarded target edge length.
    Mesh work = input;
    work.triangulate();
    work = weldCoincidentVertices(work);   // fuse unwelded coincident patches (seam fix)
    work = orientFacesConsistently(work);  // repair inconsistent face winding (robustness)
    const double area = totalSurfaceArea(work);

    // Feature-resolvability floor for the pure-quad base (spec: no silent geometry
    // loss). Quarter density can under-resolve a CLOSED sharp crease loop outright: a
    // ~100-quad base on the generated cylinder gives its rims ~10 cells, the cap cross
    // field has no room to align with the rim between its cones, and whole arcs of the
    // feature are unrepresentable no matter what downstream does. Require at least
    // kMinCellsPerCreaseLoop base cells along every closed crease loop, raising the base
    // density when the quarter-density default cannot provide them (capped at the full
    // request — the subdivision then overshoots the count honestly rather than shipping
    // a feature-less "pure" result). Open crease networks (a cube's corner-joined edges)
    // and feature-free meshes are unaffected.
    if (params.pureQuads) {
        constexpr double kMinCellsPerCreaseLoop = 18.0;
        work.tagFeatureEdges(params.sharpEdgeDegrees);
        // Connected components of interior feature edges; a component is a closed loop
        // when every touched vertex has exactly two incident feature edges.
        std::map<Index, std::vector<Index>> vertexFeatureEdges;  // vertex -> feature edges
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!work.isAlive(e) || work.edgeFaceCount(e) != 2 || !work.isFeatureEdge(e)) {
                continue;
            }
            const auto [a, b] = work.edgeVertices(e);
            vertexFeatureEdges[a.value].push_back(ei);
            vertexFeatureEdges[b.value].push_back(ei);
        }
        std::map<Index, Index> parent;  // union-find over feature vertices
        const std::function<Index(Index)> find = [&parent](Index x) {
            while (parent[x] != x) {
                x = parent[x] = parent[parent[x]];
            }
            return x;
        };
        for (const auto& [v, edges] : vertexFeatureEdges) {
            parent.emplace(v, v);
        }
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!work.isAlive(e) || work.edgeFaceCount(e) != 2 || !work.isFeatureEdge(e)) {
                continue;
            }
            const auto [a, b] = work.edgeVertices(e);
            parent[find(a.value)] = find(b.value);
        }
        std::map<Index, double> componentLength;
        std::map<Index, bool> componentClosed;
        for (const auto& [v, edges] : vertexFeatureEdges) {
            const Index root = find(v);
            auto [it, inserted] = componentClosed.emplace(root, true);
            if (edges.size() != 2) {
                it->second = false;  // junction or chain end: not a simple closed loop
            }
        }
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!work.isAlive(e) || work.edgeFaceCount(e) != 2 || !work.isFeatureEdge(e)) {
                continue;
            }
            const auto [a, b] = work.edgeVertices(e);
            componentLength[find(a.value)] +=
                static_cast<double>(length(work.position(a) - work.position(b)));
        }
        // Only loops RESOLVABLE at the quarter-density base count as features here: a
        // loop must span at least kMinResolvableCells base cells or it is sub-resolution
        // dihedral noise (an organic scan's wrinkle loops — armadillo's drove the base to
        // the full request, a 4x count overshoot, before this filter).
        constexpr double kMinResolvableCells = 8.0;
        const double baseCell =
            effectiveQuads > 0 ? std::sqrt(area / static_cast<double>(effectiveQuads)) : 0.0;
        for (const auto& [root, len] : componentLength) {
            if (!componentClosed[root] || len <= 0.0 || len < kMinResolvableCells * baseCell) {
                continue;
            }
            const double cellMax = len / kMinCellsPerCreaseLoop;
            const double floorQuads = area / (cellMax * cellMax);
            effectiveQuads = std::clamp(static_cast<int>(std::ceil(floorQuads)), effectiveQuads,
                                        std::max(effectiveQuads, params.targetQuadCount));
        }
    }
    const EdgeLengthResult lengthResult = targetEdgeLength(area, effectiveQuads, params.edgeScale);
    if (!lengthResult.ok()) {
        result.status = RunStatus::Error;
        result.error = lengthResult.error;
        return result;
    }
    // Every later stage reads `effectiveEdgeLength`, so the reported statistic
    // is the density that actually ran, not the one that was asked for.
    const ResolvedEdgeLength resolved = resolveAgainstCoordinates(work, lengthResult.edgeLength);
    const float effectiveEdgeLength = resolved.edgeLength;
    result.stats.targetEdgeLength = effectiveEdgeLength;
    if (resolved.issue) {
        result.parameterIssues.push_back(*resolved.issue);
    }

    // Stage 2: islands (face-complete, deterministic order — mesh-core spec).
    const auto islandFaces = work.islands();
    result.stats.islandCount = islandFaces.size();

    struct IslandOutcome {
        Mesh mesh;
        bool ok = false;
        std::string stage;
        std::string reason;
        std::size_t inputFaces = 0;
    };
    std::vector<IslandOutcome> outcomes(islandFaces.size());

    // Weighted progress: islands contribute proportionally to face count.
    std::size_t totalFaces = 0;
    for (const auto& faces : islandFaces) {
        totalFaces += faces.size();
    }

    float progressBase = 0.0f;
    bool fieldExtractor = false;    // did the position-field extractor run? (drives CV relax)
    bool integerExtractor = false;  // integer-grid extractor? (drives base-relax strength)
    // The quad-cover method runs its own (harness) isotropic remesh + seamless-UV solve on
    // the raw triangles; a preceding pipeline isotropic remesh would double-remesh and leave
    // the harness a density-sensitive, crack-prone input. Peek the method name once so the
    // per-island loop can skip our isotropic stage for it and feed the raw island through.
    const std::string quadMethodName =
        quadrangulator ? quadrangulator()->name() : std::string("greedy");
    const bool quadCoverMethod = quadMethodName == "quad-cover";

    // Isotropic remesh of one island in place. Returns the status; on a non-cancel
    // failure it records the stage/reason on `oc`. Factored out so the quad-cover
    // fallback path can reuse it after quad-cover declines an island.
    const auto runIsotropicStage = [&](Mesh& m, float base, float span,
                                       IslandOutcome& oc) -> IsotropicStatus {
        const ReferenceSurface reference(m, params.smoothNormalDegrees);
        IsotropicOptions iso;
        iso.targetEdgeLength = effectiveEdgeLength;
        iso.adaptivity = params.adaptivity;
        iso.smoothNormalDegrees = params.smoothNormalDegrees;
        iso.density = guidanceField.get();  // null unless painted density was supplied
        ProgressSink isoSink =
            progress ? progress->subrange(base, base + span, "isotropic") : ProgressSink{};
        const IsotropicStatus st =
            isotropicRemesh(m, reference, iso, progress ? &isoSink : nullptr, cancel);
        if (st != IsotropicStatus::Cancelled &&
            (st != IsotropicStatus::Success || m.faceCount() == 0)) {
            oc.stage = "isotropic";
            oc.reason = st == IsotropicStatus::InvalidInput
                            ? "invalid island input"
                            : "island vanished during isotropic remeshing";
        }
        return st;
    };

    for (std::size_t i = 0; i < islandFaces.size(); ++i) {
        IslandOutcome& outcome = outcomes[i];
        outcome.inputFaces = islandFaces[i].size();
        const float weight = totalFaces > 0 ? static_cast<float>(islandFaces[i].size()) /
                                                  static_cast<float>(totalFaces)
                                            : 0.0f;

        if (cancel && cancel->isCancelled()) {
            result.status = RunStatus::Cancelled;
            return result;
        }

        outcome.mesh = extractIsland(work, islandFaces[i]);
        outcome.mesh.tagFeatureEdges(params.sharpEdgeDegrees);
        // Per-island guidance audit: one row per island whenever guidance was
        // supplied, filled in as the island routes and pushed on every exit.
        IslandGuidance audit;
        audit.islandIndex = i;
        if (guidanceField) {
            Vec3 islandLo{}, islandHi{};
            bool first = true;
            for (Index vi = 0; vi < outcome.mesh.vertexCapacity(); ++vi) {
                const VertexId v{vi};
                if (!outcome.mesh.isAlive(v)) {
                    continue;
                }
                const Vec3 p = outcome.mesh.position(v);
                islandLo = first ? p : min(islandLo, p);
                islandHi = first ? p : max(islandHi, p);
                first = false;
            }
            audit.guidesInRange = guidanceField->guidesReaching(islandLo, islandHi);
        }
        const auto recordAudit = [&result, &guidanceField](IslandGuidance& row) {
            if (guidanceField) {
                result.islandGuidance.push_back(row);
            }
        };

        // Isotropic stage: overall 0.0-0.3 of this island's slice. Skipped for quad-cover,
        // which does its own isotropic remesh downstream (see quadCoverMethod above).
        if (!quadCoverMethod) {
            const IsotropicStatus isoStatus =
                runIsotropicStage(outcome.mesh, progressBase, weight * 0.3f, outcome);
            if (isoStatus == IsotropicStatus::Cancelled) {
                result.status = RunStatus::Cancelled;
                return result;
            }
            if (isoStatus != IsotropicStatus::Success || outcome.mesh.faceCount() == 0) {
                audit.reason = "island failed before quadrangulation: " + outcome.reason;
                recordAudit(audit);
                progressBase += weight;
                continue;
            }
        }

        // Quadrangulation stage: 0.3-0.9 of this island's slice. Use the
        // injected quadrangulator (field-aligned) when provided, else greedy.
        std::unique_ptr<IQuadrangulator> quad =
            quadrangulator ? quadrangulator() : makeGreedyPairingQuadrangulator();
        // instant-meshes and quad-cover both extract from a smooth field, so the
        // uniform-square shape-match relax lowers their edge-CV ~20% corpus-wide with
        // no change to irregular % and improved surface deviation (measured); only the
        // less-uniform triangle-pairing base keeps the lighter centroid relax.
        fieldExtractor = quad->name() == "instant-meshes" || quad->name() == "quad-cover";
        integerExtractor = quad->name() == "integer";
        // Offer the guidance to this island's backend. A backend that declines
        // is recorded, never silently ignored (spec: "honored loudly or
        // rejected loudly").
        if (guidanceField) {
            std::string reason;
            if (quad->acceptGuidance(*guidanceField, reason)) {
                audit.guidesHonored = guidanceField->hasGuides();
                audit.densityHonored = guidanceField->hasDensity();
            } else {
                audit.reason = reason;
            }
            // The isotropic stage sizes for density regardless of the quad
            // backend, so density is honored even where guides are not —
            // except for quad-cover, which skips our isotropic stage.
            if (!audit.densityHonored && guidanceField->hasDensity() && !quadCoverMethod) {
                audit.densityHonored = true;
            }
        }
        ProgressSink quadSink =
            progress ? progress->subrange(progressBase + weight * 0.3f,
                                          progressBase + weight * 0.9f, "quadrangulate")
                     : ProgressSink{};
        const auto quadOutcome = quad->quadrangulate(outcome.mesh, effectiveEdgeLength,
                                                     progress ? &quadSink : nullptr, cancel);
        if (quadOutcome.cancelled) {
            result.status = RunStatus::Cancelled;
            return result;
        }
        bool quadOk = quadOutcome.success && outcome.mesh.faceCount() > 0;
        const auto mergeUnhonored = [&audit](const std::vector<std::string>& reasons) {
            for (const std::string& r : reasons) {
                audit.guidesHonored = false;
                audit.reason = audit.reason.empty() ? r : audit.reason + "; " + r;
            }
        };
        mergeUnhonored(quad->unhonoredGuidance());

        // Quad-cover recovery: it skipped the isotropic stage and declined this island
        // (no seamless-UV solver, or a solve/extraction failure). The mesh is untouched,
        // so recover the island via the normal isotropic remesh + fallback quadrangulator
        // (field-aligned) — the default never loses field-aligned's always-produces-output
        // guarantee.
        if (!quadOk && quadCoverMethod && fallbackQuadrangulator) {
            const IsotropicStatus isoStatus =
                runIsotropicStage(outcome.mesh, progressBase, weight * 0.3f, outcome);
            if (isoStatus == IsotropicStatus::Cancelled) {
                result.status = RunStatus::Cancelled;
                return result;
            }
            if (isoStatus == IsotropicStatus::Success && outcome.mesh.faceCount() > 0) {
                std::unique_ptr<IQuadrangulator> fb = fallbackQuadrangulator();
                fieldExtractor = fb->name() == "instant-meshes" || fb->name() == "quad-cover";
                integerExtractor = fb->name() == "integer";
                if (guidanceField) {
                    std::string fbReason;
                    const bool accepted = fb->acceptGuidance(*guidanceField, fbReason);
                    audit.guidesHonored = accepted && guidanceField->hasGuides();
                    if (!accepted) {
                        audit.reason = fbReason;
                    } else {
                        audit.reason.clear();
                    }
                    audit.densityHonored = guidanceField->hasDensity();
                }
                const auto fbOutcome = fb->quadrangulate(outcome.mesh, effectiveEdgeLength,
                                                         progress ? &quadSink : nullptr, cancel);
                if (fbOutcome.cancelled) {
                    result.status = RunStatus::Cancelled;
                    return result;
                }
                mergeUnhonored(fb->unhonoredGuidance());
                quadOk = fbOutcome.success && outcome.mesh.faceCount() > 0;
            }
        }
        if (!quadOk) {
            if (outcome.stage.empty()) {  // not already set by a failed fallback isotropic
                outcome.stage = "quadrangulate";
                outcome.reason = quadOutcome.failureReason.empty() ? "no faces produced"
                                                                   : quadOutcome.failureReason;
            }
            audit.guidesHonored = false;
            if (audit.reason.empty()) {
                audit.reason = "island failed to quadrangulate: " + outcome.reason;
            }
            recordAudit(audit);
            progressBase += weight;
            continue;
        }
        recordAudit(audit);

        applySmallPatchPolicy(outcome.mesh, params.smallPatchPolicy, params.smallPatchMinFaces);
        outcome.ok = outcome.mesh.faceCount() > 0;
        if (!outcome.ok) {
            outcome.stage = "cleanup";
            outcome.reason = "patch policy removed all faces";
        }
        progressBase += weight;
    }

    // Stage 3: deterministic merge (island order), 0.9-1.0.
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        const IslandOutcome& outcome = outcomes[i];
        if (!outcome.ok) {
            ++result.stats.islandsFailed;
            result.failedIslands.push_back({i, outcome.inputFaces, outcome.stage, outcome.reason});
            continue;
        }
        std::vector<Vec3> islandPositions;
        std::vector<std::vector<Index>> islandFaceLists;
        outcome.mesh.toIndexed(islandPositions, islandFaceLists);
        const Index offset = static_cast<Index>(positions.size());
        positions.insert(positions.end(), islandPositions.begin(), islandPositions.end());
        for (auto& face : islandFaceLists) {
            for (Index& v : face) {
                v += offset;
            }
            faces.push_back(std::move(face));
        }
    }
    if (progress) {
        progress->report(0.95f, "merge");
    }

    result.mesh = Mesh::fromIndexed(positions, faces);
    // Cleanup: fill holes up to the boundary-length limit before the optional
    // pure-quad subdivision, so filled patches subdivide into quads too
    // (remeshing-parameters spec, "holeFillMaxBoundary"; 0/<3 disables).
    if (params.holeFillMaxBoundary >= 3 && result.mesh.faceCount() > 0) {
        result.stats.holesFilled =
            result.mesh.fillHoles(static_cast<std::size_t>(params.holeFillMaxBoundary));
    }
    if (params.pureQuads && result.mesh.faceCount() > 0) {
        // The position-field extractor produces an already-uniform base, so we
        // fit every quad to a common-sized square (shape matching) — regularising
        // 90-degree corners AND equal edge lengths at once, tightening angle and
        // edge-length CV toward the field-based reference. The default matcher's
        // base is less uniform, where forcing uniform squares would trade too
        // much angle quality, so it keeps the plain centroid relax.
        // shapeMatch (uniform-square target) equalises edge lengths, lowering CV.
        // Enabled for the position-field extractor by default; CYBER_SHAPEMATCH=1
        // forces it on for every method (Step-1 CV experiment), CYBER_SHAPEMATCH=0
        // forces it off. CYBER_RELAX_ITERS / CYBER_RELAX_LAMBDA tune the final pass.
        const GuidanceField* densityForRelax =
            (guidanceField && guidanceField->hasDensity()) ? guidanceField.get() : nullptr;
        const char* smEnv = std::getenv("CYBER_SHAPEMATCH");
        const bool shapeMatch = smEnv != nullptr ? (std::atoi(smEnv) != 0) : fieldExtractor;
        const char* riEnv = std::getenv("CYBER_RELAX_ITERS");
        const char* rlEnv = std::getenv("CYBER_RELAX_LAMBDA");
        const int finalRelaxIters = riEnv != nullptr ? std::atoi(riEnv) : 20;
        const float finalRelaxLambda =
            rlEnv != nullptr ? static_cast<float>(std::atof(rlEnv)) : 0.5f;

        // Relax the coarse base onto the source first: a skewed base subdivides
        // into skewed quads, so smoothing it before the split reduces the sliver
        // tail the subdivision would otherwise inherit. The integer extractor emits
        // a highly uniform integer-grid base whose only defect is per-cell skew, so
        // it tolerates a longer projected relax here — straightening those cells
        // before the 4x split lifts the median quad angle a couple of degrees at no
        // CV / surface-deviation cost. Less-uniform bases (field-aligned) would trade
        // edge-length CV for that, so they keep the lighter default pass.
        const bool pipeTime = std::getenv("CYBER_PIPE_TIME") != nullptr;
        using PClk = std::chrono::steady_clock;
        const auto pms = [](PClk::time_point a, PClk::time_point b) {
            // chrono::rep is long on 64-bit Linux but long long on Windows/macOS;
            // normalize to long long so the %lld format is portable.
            return static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
        };
        auto pt = PClk::now();
        // Sharp-crease polyline network of the source, shared by the base relax, the
        // post-subdivision projection and the final relax (see the crease comments
        // below). Empty when the source has no sharp edges — every use degrades to the
        // historical behavior.
        work.tagFeatureEdges(params.sharpEdgeDegrees);
        std::vector<std::array<Vec3, 2>> creaseNetwork;
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (work.isAlive(e) && work.edgeFaceCount(e) == 2 && work.isFeatureEdge(e)) {
                const auto [a, b] = work.edgeVertices(e);
                creaseNetwork.push_back({work.position(a), work.position(b)});
            }
        }
        {
            const ReferenceSurface baseSurface(work, params.smoothNormalDegrees);
            if (pipeTime) {
                std::fprintf(stderr, "[pipe-time] baseSurface build (%zu src tris)=%lldms\n",
                             work.faceCount(), pms(pt, PClk::now()));
                pt = PClk::now();
            }
            if (!baseSurface.empty()) {
                // A uniform base tolerates a longer projected relax before the 4x
                // split — it lifts median angle at no CV cost. Both the integer-grid
                // and the (Geogram/native) quad-cover seamless bases are uniform
                // enough: measured across the corpus, base=40 raises quad-cover
                // median +0.3..+1.0 deg with CV flat-to-lower and irregular %
                // unchanged. Less-uniform bases (field-aligned, position-field)
                // would trade edge-CV, so they keep the lighter pass.
                // CYBER_BASE_RELAX_ITERS overrides it for further tuning.
                const char* briEnv = std::getenv("CYBER_BASE_RELAX_ITERS");
                const int baseRelaxIters = briEnv != nullptr
                                               ? std::atoi(briEnv)
                                               : ((integerExtractor || quadCoverMethod) ? 40 : 10);
                relaxQuadMesh(result.mesh, baseSurface, params.sharpEdgeDegrees, baseRelaxIters,
                              /*lambda=*/0.5f, shapeMatch, creaseNetwork, densityForRelax);
            }
        }
        if (pipeTime) {
            std::fprintf(stderr, "[pipe-time] base relax=%lldms\n", pms(pt, PClk::now()));
            pt = PClk::now();
        }
        result.mesh = result.mesh.linearSubdivide();
        if (pipeTime) {
            std::fprintf(stderr, "[pipe-time] subdivide=%lldms\n", pms(pt, PClk::now()));
            pt = PClk::now();
        }
        // Linear subdivision only splits faces — the new vertices sit on the
        // coarse (quarter-density) base's flat facets, so the silhouette stays
        // faceted/jagged AND the split leaves many degenerate slivers. Build a
        // reference surface over the (triangulated) source, seed every vertex
        // onto it, then run tangential relaxation to de-sliver the quads while
        // following the original curvature (see relaxQuadMesh).
        const ReferenceSurface sourceSurface(work, params.smoothNormalDegrees);
        if (pipeTime) {
            std::fprintf(stderr, "[pipe-time] sourceSurface build=%lldms\n", pms(pt, PClk::now()));
            pt = PClk::now();
        }
        if (!sourceSurface.empty()) {
            // Crease vertices project onto the source CREASE POLYLINE, not the surface.
            // A closest-surface-point projection is wrong for them on a CURVED crease:
            // the coarse base samples the crease sparsely (a cylinder rim at quarter
            // density is roughly an octagon), the subdivided crease midpoints sit at the
            // chord sagitta INSIDE the surface, and their nearest surface point is on
            // the adjacent wall/cap — not the crease — so the sharp edge lands off the
            // true feature line and stays there (relaxQuadMesh freezes crease vertices).
            // Vertices are classified by DISTANCE to the network (quarter of a cell):
            // the base extraction traces creases exactly, so base crease vertices sit at
            // ~0 and subdivided crease midpoints at the (much smaller) chord sagitta,
            // while everything else is at least one subdivided cell away. A dihedral
            // re-tag cannot do this job: coarse quads on a curved wall are bent, so a
            // 90-degree crease reads ~83 degrees and goes untagged (see relaxQuadMesh).
            // Straight creases are unaffected (midpoints already lie on the line), and
            // feature-free meshes have an empty network (behavior unchanged).
            const auto creaseDistanceSq = [&creaseNetwork](const Vec3& p, Vec3* proj) {
                Vec3 best = p;
                float bestD2 = std::numeric_limits<float>::max();
                for (const auto& seg : creaseNetwork) {
                    const Vec3 ab = seg[1] - seg[0];
                    const float len2 = dot(ab, ab);
                    const float t =
                        len2 > 1e-20f ? std::clamp(dot(p - seg[0], ab) / len2, 0.0f, 1.0f) : 0.0f;
                    const Vec3 q = seg[0] + ab * t;
                    const float d2 = dot(p - q, p - q);
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        best = q;
                    }
                }
                if (proj != nullptr) {
                    *proj = best;
                }
                return bestD2;
            };
            double meanEdgeSum = 0.0;
            std::size_t meanEdgeCount = 0;
            for (Index ei = 0; ei < result.mesh.edgeCapacity(); ++ei) {
                const EdgeId e{ei};
                if (result.mesh.isAlive(e)) {
                    const auto [a, b] = result.mesh.edgeVertices(e);
                    meanEdgeSum += static_cast<double>(
                        length(result.mesh.position(a) - result.mesh.position(b)));
                    ++meanEdgeCount;
                }
            }
            // 0.7 of a (subdivided) cell: wide enough that where the coarse base's grid
            // crossed the crease DIAGONALLY (an unpinned rim segment near a cap cone
            // leaves the crease chain gapped), the crossing vertices — which sit up to
            // ~half a cell off the crease — snap onto it and close the chain; still
            // below the one-cell distance of legitimate first-ring interior vertices.
            const float onTol =
                meanEdgeCount > 0
                    ? 0.7f * static_cast<float>(meanEdgeSum / static_cast<double>(meanEdgeCount))
                    : 0.0f;
            for (Index vi = 0; vi < result.mesh.vertexCapacity(); ++vi) {
                const VertexId v{vi};
                if (!result.mesh.isAlive(v)) {
                    continue;
                }
                const Vec3 p = result.mesh.position(v);
                Vec3 creaseProj{};
                if (!creaseNetwork.empty() && onTol > 0.0f &&
                    creaseDistanceSq(p, &creaseProj) < onTol * onTol) {
                    result.mesh.setPosition(v, creaseProj);
                } else {
                    result.mesh.setPosition(v, sourceSurface.project(p));
                }
            }
            relaxQuadMesh(result.mesh, sourceSurface, params.sharpEdgeDegrees, finalRelaxIters,
                          finalRelaxLambda, shapeMatch, creaseNetwork, densityForRelax);
        }
        if (pipeTime) {
            std::fprintf(stderr, "[pipe-time] final project+relax=%lldms\n", pms(pt, PClk::now()));
        }
    }
    countFaces(result.mesh, result.stats);

    if (result.mesh.faceCount() == 0) {
        result.status = RunStatus::Error;
        result.error = "remeshing produced no result";
        return result;
    }
    result.status = result.failedIslands.empty() ? RunStatus::Success : RunStatus::Partial;
    if (progress) {
        progress->report(1.0f, "done");
    }
    return result;
}

}  // namespace cyber::remesh
