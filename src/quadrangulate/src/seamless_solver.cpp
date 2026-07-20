#include "cyber/quadrangulate/seamless_solver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/accel/buffer.hpp"
#include "cyber/accel/primitives.hpp"
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

namespace {

// The (up to 3) edges of a triangle face, found from its consecutive vertex pairs.
std::vector<EdgeId> faceEdges(const Mesh& mesh, FaceId f) {
    const std::vector<VertexId> vs = mesh.faceVertices(f);
    std::vector<EdgeId> out;
    for (std::size_t k = 0; k < vs.size(); ++k) {
        const VertexId a = vs[k];
        const VertexId b = vs[(k + 1) % vs.size()];
        for (const EdgeId e : mesh.vertexEdges(a)) {
            const auto [ea, eb] = mesh.edgeVertices(e);
            if ((ea == a && eb == b) || (ea == b && eb == a)) {
                out.push_back(e);
                break;
            }
        }
    }
    return out;
}

// Conjugate Gradient for a symmetric-positive-definite CSR system A x = b, using the
// accel spmv for the matrix-vector product (so a GPU backend accelerates it). Returns
// the iteration count; x is seeded with its incoming value.
int conjugateGradient(accel::IBackend& backend, const accel::SparseMatrix& A,
                      const std::vector<float>& b, std::vector<float>& x, int maxIters,
                      float tol) {
    const std::size_t n = A.rows;
    accel::Buffer<float> xb(std::vector<float>(x.begin(), x.end()));
    accel::Buffer<float> ax;
    accel::spmv(backend, A, xb, ax);  // Ax
    std::vector<float> r(n), p(n);
    double rs = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        r[i] = b[i] - ax[i];
        p[i] = r[i];
        rs += static_cast<double>(r[i]) * r[i];
    }
    const double rs0 = rs;
    if (rs0 <= 0.0) {
        return 0;
    }
    int it = 0;
    accel::Buffer<float> pb, apb;
    for (; it < maxIters; ++it) {
        pb.upload(p);
        accel::spmv(backend, A, pb, apb);  // Ap
        double pAp = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            pAp += static_cast<double>(p[i]) * apb[i];
        }
        if (pAp <= 0.0) {
            break;
        }
        const double alpha = rs / pAp;
        double rsNew = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += static_cast<float>(alpha) * p[i];
            r[i] -= static_cast<float>(alpha * apb[i]);
            rsNew += static_cast<double>(r[i]) * r[i];
        }
        if (rsNew <= static_cast<double>(tol) * static_cast<double>(tol) * rs0) {
            ++it;
            break;
        }
        const double beta = rsNew / rs;
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = r[i] + static_cast<float>(beta) * p[i];
        }
        rs = rsNew;
    }
    return it;
}

// Rotate direction d by r quarter-turns about axis n (r mod 4).
Vec3 rotQuarter(Vec3 d, const Vec3& n, int r) {
    const int k = ((r % 4) + 4) % 4;
    for (int i = 0; i < k; ++i) {
        d = cross(n, d);
    }
    return d;
}

// Comb the frame field: BFS over faces across non-cut interior edges, assigning each
// face an integer rotation so the combed cross is continuous across every non-cut edge.
// The cut graph absorbs the field's holonomy, so on the cut disk this is consistent.
std::vector<int> combField(const Mesh& mesh, const SeamlessSetup& setup, int& touched) {
    std::vector<int> comb(mesh.faceCapacity(), 0);
    std::vector<char> visited(mesh.faceCapacity(), 0);
    touched = 0;
    for (Index seed = 0; seed < mesh.faceCapacity(); ++seed) {
        if (!mesh.isAlive(FaceId{seed}) || visited[seed]) {
            continue;
        }
        visited[seed] = 1;
        std::queue<FaceId> q;
        q.push(FaceId{seed});
        while (!q.empty()) {
            const FaceId f = q.front();
            q.pop();
            for (const EdgeId e : faceEdges(mesh, f)) {
                if (setup.isCutEdge[e.value] || mesh.edgeFaceCount(e) != 2) {
                    continue;
                }
                const auto ef = mesh.edgeFaces(e);
                const FaceId g = (ef[0] == f) ? ef[1] : ef[0];
                if (visited[g.value]) {
                    continue;
                }
                // periodJump[e] aligns ef[1]'s cross to ef[0]'s (rotate ef[1] by +p).
                // Propagate comb so combed(g) matches combed(f).
                const int p = setup.periodJump[e.value];
                comb[g.value] = (ef[0] == f) ? (comb[f.value] - p) : (comb[f.value] + p);
                comb[g.value] = ((comb[g.value] % 4) + 4) % 4;
                if (comb[g.value] != 0) {
                    ++touched;
                }
                visited[g.value] = 1;
                q.push(g);
            }
        }
    }
    return comb;
}

}  // namespace

Parameterization solveParameterization(const Mesh& mesh, const SeamlessSetup& setup,
                                       float spacing, accel::IBackend& backend) {
    Parameterization out;
    if (!setup.valid || spacing <= 0.0f || mesh.faceCapacity() == 0) {
        return out;
    }
    const float invS = 1.0f / spacing;

    // Comb the frame so it is continuous across non-cut edges.
    int touched = 0;
    const std::vector<int> comb = combField(mesh, setup, touched);

    // Corner indexing: one node per (triangle, corner). Union-find merges the two corners
    // of a shared vertex across every NON-cut interior edge, so a cut edge (and boundary)
    // leaves the seam split. Each surviving component is one vertex of the cut-open mesh.
    std::unordered_map<std::uint64_t, std::size_t> cornerId;
    std::vector<std::size_t> parent;
    const auto ckey = [](Index f, Index v) {
        return (static_cast<std::uint64_t>(f) << 32) | static_cast<std::uint64_t>(v);
    };
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        for (const VertexId v : mesh.faceVertices(f)) {
            cornerId.emplace(ckey(fi, v.value), parent.size());
            parent.push_back(parent.size());
        }
    }
    std::function<std::size_t(std::size_t)> find = [&](std::size_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    const auto unite = [&](std::size_t a, std::size_t b) { parent[find(a)] = find(b); };
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || setup.isCutEdge[ei]) {
            continue;
        }
        const auto ef = mesh.edgeFaces(e);
        if (mesh.faceSize(ef[0]) != 3 || mesh.faceSize(ef[1]) != 3) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        unite(cornerId.at(ckey(ef[0].value, a.value)), cornerId.at(ckey(ef[1].value, a.value)));
        unite(cornerId.at(ckey(ef[0].value, b.value)), cornerId.at(ckey(ef[1].value, b.value)));
    }
    // Compact roots to cut-vertex ids 0..M-1.
    std::unordered_map<std::size_t, std::size_t> rootToCut;
    for (std::size_t i = 0; i < parent.size(); ++i) {
        rootToCut.emplace(find(i), rootToCut.size());
    }
    const std::size_t nCut = rootToCut.size();
    const auto cutOf = [&](Index f, Index v) {
        return rootToCut.at(find(cornerId.at(ckey(f, v))));
    };
    out.cutVertexCount = static_cast<int>(nCut);

    // Assemble the cotangent Laplacian L and divergence RHS b_u, b_v over the cut vertices
    // from the combed target gradients Gu = e0/spacing, Gv = e1/spacing per face.
    std::vector<std::unordered_map<std::size_t, float>> rows(nCut);
    std::vector<float> bu(nCut, 0.0f), bv(nCut, 0.0f);
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> vs = mesh.faceVertices(f);
        const std::array<Vec3, 3> p{mesh.position(vs[0]), mesh.position(vs[1]), mesh.position(vs[2])};
        const Vec3 nrm = cross(p[1] - p[0], p[2] - p[0]);
        const float area2 = length(nrm);
        if (area2 < 1e-20f) {
            continue;
        }
        const Vec3 n = nrm / area2;
        const float area = 0.5f * area2;
        const Vec3 e0 = normalized(rotQuarter(setup.field.direction(f), n, comb[fi]));
        const Vec3 e1 = cross(n, e0);
        const Vec3 gu = e0 * invS;
        const Vec3 gv = e1 * invS;
        std::array<std::size_t, 3> cut{};
        for (int k = 0; k < 3; ++k) {
            cut[static_cast<std::size_t>(k)] = cutOf(fi, vs[static_cast<std::size_t>(k)].value);
        }
        for (int k = 0; k < 3; ++k) {
            const Vec3 pk = p[static_cast<std::size_t>(k)];
            const Vec3 pa = p[static_cast<std::size_t>((k + 1) % 3)];
            const Vec3 pb = p[static_cast<std::size_t>((k + 2) % 3)];
            const Vec3 gradPhi = cross(n, pb - pa) / area2;
            bu[cut[static_cast<std::size_t>(k)]] += area * dot(gradPhi, gu);
            bv[cut[static_cast<std::size_t>(k)]] += area * dot(gradPhi, gv);
            const float cotK = dot(pa - pk, pb - pk) / area2;
            const std::size_t ia = cut[static_cast<std::size_t>((k + 1) % 3)];
            const std::size_t ib = cut[static_cast<std::size_t>((k + 2) % 3)];
            const float w = 0.5f * cotK;
            rows[ia][ib] -= w;
            rows[ib][ia] -= w;
            rows[ia][ia] += w;
            rows[ib][ib] += w;
        }
    }
    if (nCut == 0) {
        return out;
    }

    // Pin cut vertex 0 to make the Laplacian SPD for CG.
    const std::size_t pin = 0;
    accel::SparseMatrix A;
    A.rows = nCut;
    A.rowStart.reserve(nCut + 1);
    A.rowStart.push_back(0);
    for (std::size_t i = 0; i < nCut; ++i) {
        if (i == pin || rows[i].empty()) {
            A.colIndex.push_back(i);
            A.value.push_back(1.0f);
            bu[i] = (i == pin) ? 0.0f : bu[i];
            bv[i] = (i == pin) ? 0.0f : bv[i];
            if (rows[i].empty() && i != pin) {
                bu[i] = 0.0f;
                bv[i] = 0.0f;
            }
            A.rowStart.push_back(A.colIndex.size());
            continue;
        }
        for (const auto& [j, w] : rows[i]) {
            if (j == pin) {
                continue;  // pinned to 0 -> no RHS contribution
            }
            A.colIndex.push_back(j);
            A.value.push_back(w);
        }
        A.rowStart.push_back(A.colIndex.size());
    }
    bu[pin] = 0.0f;
    bv[pin] = 0.0f;

    std::vector<float> u(nCut, 0.0f), v(nCut, 0.0f);
    const int maxIters = static_cast<int>(nCut) + 500;
    out.cgIterationsU = conjugateGradient(backend, A, bu, u, maxIters, 1e-8f);
    out.cgIterationsV = conjugateGradient(backend, A, bv, v, maxIters, 1e-8f);

    // Per-corner UV.
    out.cornerUv.assign(mesh.faceCapacity(), std::array<Vec2, 3>{});
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> vs = mesh.faceVertices(f);
        for (int k = 0; k < 3; ++k) {
            const std::size_t cv = cutOf(fi, vs[static_cast<std::size_t>(k)].value);
            out.cornerUv[fi][static_cast<std::size_t>(k)] = Vec2{u[cv], v[cv]};
        }
    }
    out.valid = true;
    return out;
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
