#include "cyber/quadrangulate/seamless_solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"

// Native QuadCover seamless-UV — Milestone 1 (docs/native-miq-plan.md): the frame-field
// setup a seamless parameterization is solved on. Reuses the per-face CrossField; adds
// period jumps, cross-field singularity indices (validated by Poincare-Hopf: the indices
// sum to 4 * Euler characteristic), and a cut graph that opens the surface to a disk.
namespace cyber::remesh {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;

// Angle of unit direction `d` in the face frame (tangent, bitangent).
float frameAngle(const Vec3& d, const Vec3& tangent, const Vec3& bitangent) {
    return std::atan2(dot(d, bitangent), dot(d, tangent));
}

// Wrap x into (-pi/4, pi/4] — the minimal residual of a 4-RoSy (90-degree) rotation.
float wrapQuarter(float x) {
    while (x > kHalfPi * 0.5f) {
        x -= kHalfPi;
    }
    while (x <= -kHalfPi * 0.5f) {
        x += kHalfPi;
    }
    return x;
}

// Cross angle of face f relative to a shared edge whose world direction is `d`:
// (cross angle in the face frame) - (edge angle in the face frame).
float crossRelEdge(const CrossField& field, Index f, const Vec3& d) {
    const float theta = field.angle(FaceId{f});
    return theta - frameAngle(d, field.tangent[f], field.bitangent[f]);
}

// Interior angle of face f at vertex v.
float interiorAngle(const Mesh& mesh, FaceId f, VertexId v) {
    const std::vector<VertexId> vs = mesh.faceVertices(f);
    std::size_t at = vs.size();
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (vs[i] == v) {
            at = i;
            break;
        }
    }
    if (at == vs.size()) {
        return 0.0f;
    }
    const Vec3 p = mesh.position(v);
    const Vec3 a = mesh.position(vs[(at + vs.size() - 1) % vs.size()]);
    const Vec3 b = mesh.position(vs[(at + 1) % vs.size()]);
    const Vec3 da = normalized(a - p);
    const Vec3 db = normalized(b - p);
    return std::acos(std::clamp(dot(da, db), -1.0f, 1.0f));
}

// Period-jump residual across edge e (shared by two faces), measured with the edge
// oriented a->b: returns {integer jump r in {0,1,2,3}, residual in (-pi/4, pi/4]}.
std::pair<int, float> edgeJump(const Mesh& mesh, const CrossField& field, EdgeId e) {
    const auto ef = mesh.edgeFaces(e);
    if (ef.size() != 2) {
        return {0, 0.0f};
    }
    const auto [a, b] = mesh.edgeVertices(e);
    const Vec3 d = normalized(mesh.position(b) - mesh.position(a));
    const float a0 = crossRelEdge(field, ef[0].value, d);
    const float a1 = crossRelEdge(field, ef[1].value, d);
    const float raw = a1 - a0;
    const int r = ((static_cast<int>(std::lround(raw / kHalfPi)) % 4) + 4) % 4;
    return {r, wrapQuarter(raw)};
}

// In triangle face f, the vertex following v in the face's CCW order (the head of the
// CCW-outgoing spoke from v). Null if v is not a corner of f.
VertexId ccwNext(const Mesh& mesh, FaceId f, VertexId v) {
    const std::vector<VertexId> vs = mesh.faceVertices(f);
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (vs[i] == v) {
            return vs[(i + 1) % vs.size()];
        }
    }
    return VertexId{kInvalidIndex};
}

// Cross-field index at interior vertex v. Walks the one-ring in a consistent CCW
// direction (following each face's own orientation), accumulating the field's minimal
// rotation across each spoke plus the geometric angle defect; index = round(total /
// (pi/2)). Correct orientation is what makes the indices sum to 4*chi (Poincare-Hopf).
// Returns 0 for a boundary / non-manifold / degenerate one-ring.
int vertexIndex(const Mesh& mesh, const CrossField& field, VertexId v) {
    const std::size_t valence = mesh.vertexFaces(v).size();
    if (valence < 3) {
        return 0;
    }
    const FaceId start = mesh.vertexFaces(v)[0];
    float residual = 0.0f;
    float angleSum = 0.0f;
    FaceId f = start;
    for (std::size_t step = 0; step < valence; ++step) {
        angleSum += interiorAngle(mesh, f, v);
        // CCW-outgoing spoke of f at v, and the face across it.
        const VertexId w = ccwNext(mesh, f, v);
        if (w.value == kInvalidIndex) {
            return 0;
        }
        EdgeId spoke{kInvalidIndex};
        for (const EdgeId e : mesh.vertexEdges(v)) {
            const auto [a, b] = mesh.edgeVertices(e);
            if ((a == v && b == w) || (a == w && b == v)) {
                spoke = e;
                break;
            }
        }
        if (spoke.value == kInvalidIndex) {
            return 0;
        }
        const auto ef = mesh.edgeFaces(spoke);
        if (ef.size() != 2) {
            return 0;  // boundary spoke -> v is a boundary vertex
        }
        const FaceId fNext = (ef[0] == f) ? ef[1] : ef[0];
        // Field's minimal rotation across the spoke, edge oriented v -> w so both faces
        // are measured against the same physical direction.
        const Vec3 d = normalized(mesh.position(w) - mesh.position(v));
        const float aCur = crossRelEdge(field, f.value, d);
        const float aNext = crossRelEdge(field, fNext.value, d);
        // aCur - aNext (not aNext - aCur): the residual must carry the same rotational
        // sense as the geometric defect so the indices sum to +4*chi (Poincare-Hopf),
        // not -4*chi.
        residual += wrapQuarter(aCur - aNext);
        f = fNext;
    }
    if (f != start) {
        return 0;  // ring did not close
    }
    const float defect = 2.0f * kPi - angleSum;
    return static_cast<int>(std::lround((residual + defect) / kHalfPi));
}

}  // namespace

std::size_t SeamlessSetup::singularityCount() const {
    return static_cast<std::size_t>(
        std::count_if(singularityIndex.begin(), singularityIndex.end(), [](int k) { return k != 0; }));
}

int SeamlessSetup::totalIndex() const {
    return std::accumulate(singularityIndex.begin(), singularityIndex.end(), 0);
}

SeamlessSetup buildSeamlessSetup(const Mesh& mesh, int iterations, accel::IBackend& backend) {
    SeamlessSetup setup;
    if (mesh.faceCapacity() == 0) {
        return setup;
    }
    setup.field = computeCrossField(mesh, iterations, backend);

    // Per-edge period jumps.
    setup.periodJump.assign(mesh.edgeCapacity(), 0);
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (mesh.isAlive(e) && mesh.edgeFaceCount(e) == 2 && !mesh.isFeatureEdge(e)) {
            setup.periodJump[ei] = edgeJump(mesh, setup.field, e).first;
        }
    }

    // Per-vertex singularity index.
    setup.singularityIndex.assign(mesh.vertexCapacity(), 0);
    std::vector<VertexId> singular;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const int k = vertexIndex(mesh, setup.field, v);
        setup.singularityIndex[vi] = k;
        if (k != 0) {
            singular.push_back(v);
        }
    }

    // Cut graph: a spanning tree over the singular vertices, routed along mesh edges via
    // BFS from the growing tree. Slitting a closed surface along this tree opens it to a
    // disk (validated by cutOpenEulerCharacteristic).
    setup.isCutEdge.assign(mesh.edgeCapacity(), false);
    if (singular.size() >= 2) {
        std::vector<char> inTree(mesh.vertexCapacity(), 0);
        std::vector<char> isSingular(mesh.vertexCapacity(), 0);
        for (const VertexId s : singular) {
            isSingular[s.value] = 1;
        }
        inTree[singular[0].value] = 1;
        std::size_t connected = 1;
        while (connected < singular.size()) {
            // BFS from all in-tree vertices to the nearest not-yet-connected singular one.
            std::vector<Index> parentEdge(mesh.vertexCapacity(), kInvalidIndex);
            std::vector<char> visited(mesh.vertexCapacity(), 0);
            std::queue<VertexId> q;
            for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
                if (inTree[vi]) {
                    visited[vi] = 1;
                    q.push(VertexId{vi});
                }
            }
            VertexId target{kInvalidIndex};
            while (!q.empty() && target.value == kInvalidIndex) {
                const VertexId u = q.front();
                q.pop();
                for (const EdgeId e : mesh.vertexEdges(u)) {
                    if (!mesh.isAlive(e)) {
                        continue;
                    }
                    const auto [a, b] = mesh.edgeVertices(e);
                    const VertexId w = (a == u) ? b : a;
                    if (visited[w.value]) {
                        continue;
                    }
                    visited[w.value] = 1;
                    parentEdge[w.value] = e.value;
                    if (isSingular[w.value] && !inTree[w.value]) {
                        target = w;
                        break;
                    }
                    q.push(w);
                }
            }
            if (target.value == kInvalidIndex) {
                break;  // disconnected component; stop (still a valid partial cut)
            }
            // Walk the parent chain back, marking cut edges and adding the path to the tree.
            VertexId w = target;
            while (parentEdge[w.value] != kInvalidIndex) {
                const EdgeId e{parentEdge[w.value]};
                setup.isCutEdge[e.value] = true;
                inTree[w.value] = 1;
                const auto [a, b] = mesh.edgeVertices(e);
                w = (a == w) ? b : a;
            }
            inTree[target.value] = 1;
            ++connected;
        }
    }

    setup.valid = true;
    return setup;
}

int cutOpenEulerCharacteristic(const Mesh& mesh, const SeamlessSetup& setup) {
    // Union-find over (face, corner) so corners of the same vertex merge across every
    // interior edge that is NOT cut; each surviving component is a vertex of the cut mesh.
    std::vector<std::pair<Index, Index>> corners;  // (face, vertex)
    std::unordered_map<std::uint64_t, std::size_t> cornerId;
    const auto key = [](Index f, Index v) {
        return (static_cast<std::uint64_t>(f) << 32) | static_cast<std::uint64_t>(v);
    };
    std::size_t nFaces = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        ++nFaces;
        for (const VertexId v : mesh.faceVertices(f)) {
            cornerId.emplace(key(fi, v.value), corners.size());
            corners.push_back({fi, v.value});
        }
    }
    std::vector<std::size_t> parent(corners.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    std::function<std::size_t(std::size_t)> find = [&](std::size_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    const auto unite = [&](std::size_t a, std::size_t b) { parent[find(a)] = find(b); };

    std::size_t nEdges = 0;
    std::size_t nCutInterior = 0;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e)) {
            continue;
        }
        ++nEdges;
        const auto ef = mesh.edgeFaces(e);
        if (ef.size() != 2) {
            continue;  // boundary edge: no merge
        }
        if (setup.isCutEdge[ei]) {
            ++nCutInterior;  // cut edge is doubled, and does not merge its corners
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        unite(cornerId.at(key(ef[0].value, a.value)), cornerId.at(key(ef[1].value, a.value)));
        unite(cornerId.at(key(ef[0].value, b.value)), cornerId.at(key(ef[1].value, b.value)));
    }

    std::size_t nVerts = 0;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        if (find(i) == i) {
            ++nVerts;
        }
    }
    const std::size_t eCut = nEdges + nCutInterior;  // each cut interior edge splits in two
    return static_cast<int>(nVerts) - static_cast<int>(eCut) + static_cast<int>(nFaces);
}

}  // namespace cyber::remesh
