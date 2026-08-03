#include "cyber/core/region_solve.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace cyber::remesh {

namespace {

// Quantized position key for coincident-duplicate detection. weldCoincidentVertices
// (pipeline.cpp:67-119) cannot run in region mode — it round-trips through
// Mesh::fromIndexed and would renumber the very ids the guarantee rests on — so
// the condition it repairs is detected and refused instead of silently skipped.
struct PositionKey {
    std::int64_t x, y, z;
    friend bool operator==(const PositionKey& a, const PositionKey& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};
struct PositionKeyHash {
    std::size_t operator()(const PositionKey& k) const {
        std::size_t h = static_cast<std::size_t>(k.x) * 0x9e3779b97f4a7c15ULL;
        h ^= static_cast<std::size_t>(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

PositionKey quantize(Vec3 p, float scale) {
    const auto q = [scale](float v) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(v) / scale));
    };
    return {q(p.x), q(p.y), q(p.z)};
}

// Diagonal of the region's bounding box, for a scale-free coincidence tolerance.
float regionScale(const Mesh& mesh, const std::vector<VertexId>& verts) {
    if (verts.empty()) {
        return 1.0f;
    }
    Vec3 lo = mesh.position(verts.front()), hi = lo;
    for (const VertexId v : verts) {
        const Vec3 p = mesh.position(v);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const float diag = length(hi - lo);
    return diag > 0.0f ? diag : 1.0f;
}

// The directed edge (a,b) may appear at most once across a manifold mesh's
// faces; a second occurrence means two faces wind the shared edge the same way.
// orientFacesConsistently (pipeline.cpp:130-220) is likewise unavailable here.
bool hasInconsistentWinding(const Mesh& mesh) {
    std::unordered_set<std::uint64_t> seen;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> ring = mesh.faceVertices(f);
        for (std::size_t k = 0; k < ring.size(); ++k) {
            const std::uint64_t a = ring[k].value;
            const std::uint64_t b = ring[(k + 1) % ring.size()].value;
            if (!seen.insert((a << 32) | b).second) {
                return true;
            }
        }
    }
    return false;
}

// The unique OTHER interface edge at `v`, or invalid when the interface does
// not continue simply through it. Mirrors detail::nextBoundaryEdge in
// retopo/boundary.hpp, including its deterministic stop at a pinch.
EdgeId nextInterfaceEdge(const Mesh& mesh, const RegionSolve& region, VertexId v, EdgeId current) {
    EdgeId next{};
    int count = 0;
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (e.value == current.value || !region.isInterfaceEdge(mesh, e)) {
            continue;
        }
        ++count;
        next = e;
    }
    return count == 1 ? next : EdgeId{};
}

VertexId otherVertex(const Mesh& mesh, EdgeId e, VertexId v) {
    const auto [a, b] = mesh.edgeVertices(e);
    return a.value == v.value ? b : a;
}

}  // namespace

std::string_view describe(RegionSolveStatus status) {
    switch (status) {
        case RegionSolveStatus::Ok:
            return "ok";
        case RegionSolveStatus::EmptyRegion:
            return "region names no faces";
        case RegionSolveStatus::InvalidFace:
            return "region names a dead, out-of-range or repeated face";
        case RegionSolveStatus::WholeMesh:
            return "region is the whole mesh; use the whole-mesh solve";
        case RegionSolveStatus::Disconnected:
            return "region faces are not face-connected";
        case RegionSolveStatus::CoincidentVertices:
            return "source has coincident duplicate vertices; weld the document first";
        case RegionSolveStatus::InconsistentWinding:
            return "source has inconsistent face winding; repair the document first";
    }
    return "unknown";
}

bool RegionSolve::isInterfaceEdge(const Mesh& mesh, EdgeId e) const {
    if (!mesh.isAlive(e)) {
        return false;
    }
    const std::vector<FaceId> faces = mesh.edgeFaces(e);
    if (faces.size() != 2) {
        return false;  // a real mesh boundary is not a region interface
    }
    return frozen(faces[0]) != frozen(faces[1]);
}

RegionSolveResult buildRegionSolve(Mesh& mesh, std::span<const FaceId> regionFaces,
                                   float sharpEdgeDegrees,
                                   const std::unordered_map<Index, int>* valenceOverrides) {
    RegionSolveResult result;
    if (regionFaces.empty()) {
        result.status = RegionSolveStatus::EmptyRegion;
        return result;
    }

    // (a) Validate the caller's face set BEFORE mutating anything.
    std::vector<char> inRegion(mesh.faceCapacity(), 0);
    std::size_t named = 0;
    for (const FaceId f : regionFaces) {
        if (f.value >= inRegion.size() || !mesh.isAlive(f) || inRegion[f.value] != 0) {
            result.status = RegionSolveStatus::InvalidFace;
            return result;
        }
        inRegion[f.value] = 1;
        ++named;
    }
    if (named == mesh.faceCount()) {
        // Deliberately refused rather than aliased: the region path skips
        // weld/orient/islands/pureQuads and preserves ids, so it is NOT the
        // same operation as a whole-mesh solve.
        result.status = RegionSolveStatus::WholeMesh;
        return result;
    }

    // (b) Face-connectivity of the region, by BFS over shared edges.
    {
        std::vector<Index> stack{regionFaces.front().value};
        std::vector<char> seen(mesh.faceCapacity(), 0);
        seen[regionFaces.front().value] = 1;
        std::size_t reached = 1;
        while (!stack.empty()) {
            const FaceId f{stack.back()};
            stack.pop_back();
            const std::vector<VertexId> ring = mesh.faceVertices(f);
            for (std::size_t k = 0; k < ring.size(); ++k) {
                const VertexId a = ring[k];
                const VertexId b = ring[(k + 1) % ring.size()];
                for (const EdgeId e : mesh.vertexEdges(a)) {
                    const auto [x, y] = mesh.edgeVertices(e);
                    const bool matches = (x.value == a.value && y.value == b.value) ||
                                         (x.value == b.value && y.value == a.value);
                    if (!matches) {
                        continue;
                    }
                    for (const FaceId g : mesh.edgeFaces(e)) {
                        if (g.value < inRegion.size() && inRegion[g.value] != 0 &&
                            seen[g.value] == 0) {
                            seen[g.value] = 1;
                            ++reached;
                            stack.push_back(g.value);
                        }
                    }
                    break;
                }
            }
        }
        if (reached != named) {
            result.status = RegionSolveStatus::Disconnected;
            return result;
        }
    }

    // (c) Hostile inputs the region path cannot repair, refused up front.
    {
        std::vector<VertexId> live;
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (mesh.isAlive(v)) {
                live.push_back(v);
            }
        }
        const float tolerance = regionScale(mesh, live) * 1e-6f;
        std::unordered_set<PositionKey, PositionKeyHash> seen;
        seen.reserve(live.size() * 2);
        for (const VertexId v : live) {
            if (!seen.insert(quantize(mesh.position(v), tolerance)).second) {
                result.status = RegionSolveStatus::CoincidentVertices;
                return result;
            }
        }
        if (hasInconsistentWinding(mesh)) {
            result.status = RegionSolveStatus::InconsistentWinding;
            return result;
        }
    }

    // ---- past this point the mesh IS mutated ----
    RegionSolve& region = result.region;

    // (d) frozen mask: every alive face NOT named by the caller.
    region.frozenFace.assign(mesh.faceCapacity(), 0);
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (mesh.isAlive(f) && inRegion[i] == 0) {
            region.frozenFace[i] = 1;
        }
    }

    // (e) pinned mask, computed BEFORE triangulation so it is keyed on the
    // caller's vertices; triangulation adds no vertices, so it stays complete.
    region.vertexPinned.assign(mesh.vertexCapacity(), 0);
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || !region.frozen(f)) {
            continue;
        }
        for (const VertexId v : mesh.faceVertices(f)) {
            if (v.value < region.vertexPinned.size()) {
                region.vertexPinned[v.value] = 1;
            }
        }
    }

    // (f) Interface rings, and the prescription, BEFORE the region is
    // triangulated — the cage-implied valence is a property of the input.
    std::vector<char> interfaceVertex(mesh.vertexCapacity(), 0);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v) || !region.pinned(v)) {
            continue;
        }
        int frozenIncident = 0, activeIncident = 0;
        for (const FaceId f : mesh.vertexFaces(v)) {
            (region.frozen(f) ? frozenIncident : activeIncident) += 1;
        }
        if (activeIncident == 0) {
            continue;  // deep in the frozen complement, not on the interface
        }
        interfaceVertex[i] = 1;
        bool onMeshBoundary = false;
        for (const EdgeId e : mesh.vertexEdges(v)) {
            if (mesh.edgeFaces(e).size() == 1) {
                onMeshBoundary = true;
                break;
            }
        }
        int total = onMeshBoundary ? 2 : 4;
        if (valenceOverrides != nullptr) {
            const auto it = valenceOverrides->find(i);
            if (it != valenceOverrides->end()) {
                total = it->second;
            }
        }
        region.targetValence[i] = total;
        region.requiredInRegion[i] = total - frozenIncident;
    }

    // Ordered walks over the interface, ascending EdgeId for determinism.
    {
        std::unordered_set<Index> consumed;
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            const EdgeId seed{i};
            if (!region.isInterfaceEdge(mesh, seed) || consumed.count(i) != 0) {
                continue;
            }
            consumed.insert(i);
            const auto [first, second] = mesh.edgeVertices(seed);
            std::vector<VertexId> ring{first, second};
            VertexId cursor = second;
            EdgeId arrived = seed;
            for (Index step = 0; step < mesh.edgeCapacity(); ++step) {
                const EdgeId next = nextInterfaceEdge(mesh, region, cursor, arrived);
                if (!next.valid() || consumed.count(next.value) != 0) {
                    break;
                }
                consumed.insert(next.value);
                cursor = otherVertex(mesh, next, cursor);
                if (cursor.value == first.value) {
                    break;  // closed
                }
                ring.push_back(cursor);
                arrived = next;
            }
            region.interfaceLoops.push_back(std::move(ring));
        }
    }

    // Interior index budget: 4*chi - sum(2 - q_in) over the interface, with chi
    // taken over the region-incident elements. Reported, never enforced.
    {
        std::unordered_set<Index> rv, re;
        std::size_t rf = 0;
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!mesh.isAlive(f) || region.frozen(f)) {
                continue;
            }
            ++rf;
            const std::vector<VertexId> ring = mesh.faceVertices(f);
            for (std::size_t k = 0; k < ring.size(); ++k) {
                rv.insert(ring[k].value);
                const VertexId a = ring[k];
                const VertexId b = ring[(k + 1) % ring.size()];
                for (const EdgeId e : mesh.vertexEdges(a)) {
                    const auto [x, y] = mesh.edgeVertices(e);
                    if ((x.value == a.value && y.value == b.value) ||
                        (x.value == b.value && y.value == a.value)) {
                        re.insert(e.value);
                        break;
                    }
                }
            }
        }
        const int chi = static_cast<int>(rv.size()) - static_cast<int>(re.size()) +
                        static_cast<int>(rf);
        int charge = 0;
        for (const auto& [idx, qin] : region.requiredInRegion) {
            (void)idx;
            charge += 2 - qin;
        }
        region.interiorIndexBudget = 4 * chi - charge;
    }

    // (g) Region-scoped triangulation. triangulateFace only calls splitFace
    // (mesh_ops.cpp:353-364) — it never touches m_vertices, so no prescribed
    // position can move here. The whole-mesh work.triangulate() is skipped.
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (mesh.isAlive(f) && region.active(f) && mesh.faceSize(f) > 3) {
            mesh.triangulateFace(f);
        }
    }

    // (h) Feature tagging, in this order. tagFeatureEdges rewrites `feature` on
    // EVERY alive edge, so running it after the interface tagging would erase it.
    mesh.tagFeatureEdges(sharpEdgeDegrees);
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!mesh.isAlive(e)) {
            continue;
        }
        for (const FaceId f : mesh.edgeFaces(e)) {
            if (region.frozen(f)) {
                // Covers both the interface (one active + one frozen face, so
                // edgeFaceCount == 2 and isBoundaryEdge is FALSE) and every
                // interior edge of the frozen complement.
                mesh.setFeatureEdge(e, true);
                break;
            }
        }
    }
    return result;
}

}  // namespace cyber::remesh
