#include "cyber/quadrangulate/seamless_solver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    // Experimental: derive the cross field from the multiresolution per-vertex
    // orientation (coarse-to-fine singularity placement) instead of single-level
    // face smoothing. Gated so the stock seamless path is unchanged.
    if (std::getenv("CYBER_QC_CROSSFIELD_MULTIRES") != nullptr) {
        setup.field = computeCrossFieldFromOrientation(mesh, iterations);
    } else {
        setup.field = computeCrossField(mesh, iterations, backend);
    }

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

    // Feature edges are HARD seams. A sharp crease is a place the integer grid must break so
    // its isolines run ALONG the feature (the cross field is already feature-aligned, so both
    // sides carry a grid parallel/perpendicular to the crease). Marking every interior feature
    // edge as a cut edge splits the corners there (the seam absorbs the field's discontinuity
    // across the crease) instead of forcing a continuous UV over a ~45-degree field mismatch —
    // which is what made CAD meshes ill-conditioned and divergent. This is what lets feature
    // meshes run the native solve at all (replacing the old "decline if any feature" gate).
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (mesh.isAlive(e) && mesh.edgeFaceCount(e) == 2 && mesh.isFeatureEdge(e)) {
            setup.isCutEdge[e.value] = true;
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
                      float tol, const CancelToken* cancel = nullptr) {
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
        if (cancel != nullptr && (it & 63) == 0 && cancel->isCancelled()) {
            break;
        }
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

// Coefficients of R^rho applied to a (u,v) pair, R = CCW (u,v) -> (-v,u):
//   (R^rho (u,v))_x = cxu*u + cxv*v ;  (R^rho (u,v))_y = cyu*u + cyv*v.
void rotCoeffs(int rho, double& cxu, double& cxv, double& cyu, double& cyv) {
    switch (((rho % 4) + 4) % 4) {
        case 0: cxu = 1; cxv = 0; cyu = 0; cyv = 1; break;
        case 1: cxu = 0; cxv = -1; cyu = 1; cyv = 0; break;
        case 2: cxu = -1; cxv = 0; cyu = 0; cyv = -1; break;
        default: cxu = 0; cxv = 1; cyu = -1; cyv = 0; break;
    }
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

// One cut (seam) edge, in cut-vertex ids: endpoints a,b each appear as a copy on side A
// (face ef[0]) and side B (face ef[1]); rho is the quarter-turn relating side A's UV frame
// to side B's, so uv_B = R^rho uv_A + t across this edge.
struct SeamRef {
    std::size_t aA = 0, bA = 0, aB = 0, bB = 0;
    int rho = 0;
};

// Sparse constraint-elimination MIQ (docs/native-miq-plan.md, "M2c-sparse"). The seam
// rigidity uv_B = R^rho uv_A + t (t an integer 2-vector shared by both endpoints of a seam
// edge) plus the gauge pin are HOMOGENEOUS linear constraints ONCE the integer translations
// are promoted to extra variables z = [u | v | t]. Every rotation/translation coefficient is
// +-1, so exact Gauss-Jordan elimination reduces z to independent DOF w with a sparse
// reduction map z = T w. This automatically reconciles branch-point holonomy: a regular
// cut-junction closes to a pure-integer row that eliminates one redundant translation, and a
// cone closes to a pin of its representative. The surviving independent integers are therefore
// UNCONSTRAINED -- rounding them to ANY integers keeps the map seamless (the dependent integers
// stay integer as +-1 combinations, cones land on the half-integer lattice). We minimise the
// reduced Dirichlet energy (CG on T^T blkdiag(L,L) T via accel::spmv), greedily round the
// integers (round the most-confident, fix it, re-solve), and reconstruct z = T w. Scales to
// hundreds of singularities: no dense dual, no seam cap. Returns total CG iterations.
int solveSeamlessReduced(accel::IBackend& backend, std::size_t nCut,
                         const std::vector<std::unordered_map<std::size_t, float>>& rows,
                         const std::vector<float>& bu, const std::vector<float>& bv,
                         const std::vector<SeamRef>& seams, const std::vector<std::size_t>& gauges,
                         std::vector<float>& u, std::vector<float>& v,
                         const CancelToken* cancel = nullptr) {
    const std::size_t nSeam = seams.size();
    const std::size_t nUv = 2 * nCut;
    const std::size_t N = nUv + 2 * nSeam;
    const auto uIx = [](std::size_t c) { return c; };
    const auto vIx = [nCut](std::size_t c) { return nCut + c; };
    const auto txIx = [nCut](std::size_t e) { return 2 * nCut + 2 * e; };
    const auto tyIx = [nCut](std::size_t e) { return 2 * nCut + 2 * e + 1; };

    using Row = std::unordered_map<std::size_t, double>;
    const auto addC = [](Row& r, std::size_t c, double w) {
        if (w != 0.0) {
            r[c] += w;
        }
    };
    const double eps = 1e-7;
    const auto prune = [&](Row& r) {
        for (auto it = r.begin(); it != r.end();) {
            if (std::abs(it->second) < eps) {
                it = r.erase(it);
            } else {
                ++it;
            }
        }
    };

    // Homogeneous constraint rows (== 0): 4 per seam edge (x,y at each endpoint) + gauge.
    std::vector<Row> cons;
    cons.reserve(4 * nSeam + 2 * gauges.size());
    for (std::size_t e = 0; e < nSeam; ++e) {
        const SeamRef& s = seams[e];
        double cxu, cxv, cyu, cyv;
        rotCoeffs(s.rho, cxu, cxv, cyu, cyv);
        const std::array<std::pair<std::size_t, std::size_t>, 2> ends{
            std::make_pair(s.aA, s.aB), std::make_pair(s.bA, s.bB)};
        for (const auto& [pA, pB] : ends) {
            Row rx, ry;
            addC(rx, uIx(pB), 1.0);
            addC(rx, uIx(pA), -cxu);
            addC(rx, vIx(pA), -cxv);
            addC(rx, txIx(e), -1.0);
            addC(ry, vIx(pB), 1.0);
            addC(ry, uIx(pA), -cyu);
            addC(ry, vIx(pA), -cyv);
            addC(ry, tyIx(e), -1.0);
            prune(rx);
            prune(ry);
            if (!rx.empty()) {
                cons.push_back(std::move(rx));
            }
            if (!ry.empty()) {
                cons.push_back(std::move(ry));
            }
        }
    }
    // Gauge: pin u and v to 0 at ONE vertex PER CONNECTED COMPONENT of the cut-open mesh. The
    // reduced Dirichlet operator inherits the cotan Laplacian's per-component constant nullspace,
    // so each component needs its own translation pin or the CG drifts arbitrarily far along that
    // nullspace (the ~1e9-cell blow-up the divergence guard rejected). Feature seams fragment the
    // surface into several patches, so a single global gauge is not enough.
    for (const std::size_t gauge : gauges) {
        Row rg;
        addC(rg, uIx(gauge), 1.0);
        cons.push_back(std::move(rg));
        Row rh;
        addC(rh, vIx(gauge), 1.0);
        cons.push_back(std::move(rh));
    }

    // Gauss-Jordan elimination. pivotExpr[j] (valid where isPivot[j]) gives dependent variable
    // j as an affine-free combination of the surviving independent variables. Pivots prefer a
    // CONTINUOUS variable (so integer translations stay free to be rounded); a unit coefficient
    // is preferred to keep the elimination integer-exact.
    std::vector<char> isPivot(N, 0);
    std::vector<Row> pivotExpr(N);
    for (Row& raw : cons) {
        Row cur;
        for (const auto& [c, w] : raw) {
            if (isPivot[c]) {
                for (const auto& [c2, w2] : pivotExpr[c]) {
                    cur[c2] += w * w2;
                }
            } else {
                cur[c] += w;
            }
        }
        prune(cur);
        if (cur.empty()) {
            continue;  // redundant (holonomy already reconciled)
        }
        std::size_t pv = N;
        int bestScore = -1;
        double bestMag = 0.0;
        for (const auto& [c, w] : cur) {
            const bool cont = c < nUv;
            const bool unit = std::abs(std::abs(w) - 1.0) < 1e-6;
            const int score = (cont ? 2 : 0) + (unit ? 1 : 0);
            if (score > bestScore || (score == bestScore && std::abs(w) > bestMag)) {
                bestScore = score;
                bestMag = std::abs(w);
                pv = c;
            }
        }
        const double cpv = cur[pv];
        Row expr;
        for (const auto& [c, w] : cur) {
            if (c != pv) {
                expr[c] = -w / cpv;
            }
        }
        // Back-substitute the new pivot into every existing pivot expression (Gauss-Jordan), so
        // all pivot expressions stay in independent variables only.
        for (std::size_t k = 0; k < N; ++k) {
            if (!isPivot[k]) {
                continue;
            }
            auto it = pivotExpr[k].find(pv);
            if (it == pivotExpr[k].end()) {
                continue;
            }
            const double coeff = it->second;
            pivotExpr[k].erase(it);
            for (const auto& [c, w] : expr) {
                pivotExpr[k][c] += coeff * w;
            }
            prune(pivotExpr[k]);
        }
        isPivot[pv] = 1;
        pivotExpr[pv] = std::move(expr);
    }

    // Independent variables -> reduced index; classify integer (translation) frees for rounding.
    std::vector<std::size_t> freeIx(N, kInvalidIndex);
    std::vector<std::size_t> intFree;  // reduced indices of independent integer translations
    std::size_t W = 0;
    for (std::size_t j = 0; j < N; ++j) {
        if (!isPivot[j]) {
            freeIx[j] = W++;
            if (j >= nUv) {
                intFree.push_back(freeIx[j]);
            }
        }
    }

    // Reduction map on the UV block only: Tuv (nUv x W) and its transpose Tt (W x nUv).
    const auto buildCsr = [](std::size_t nr, const std::vector<std::vector<std::pair<std::size_t, double>>>& r) {
        accel::SparseMatrix m;
        m.rows = nr;
        m.rowStart.reserve(nr + 1);
        m.rowStart.push_back(0);
        for (std::size_t i = 0; i < nr; ++i) {
            for (const auto& [c, w] : r[i]) {
                m.colIndex.push_back(c);
                m.value.push_back(static_cast<float>(w));
            }
            m.rowStart.push_back(m.colIndex.size());
        }
        return m;
    };
    std::vector<std::vector<std::pair<std::size_t, double>>> tuvRows(nUv), ttRows(W);
    for (std::size_t j = 0; j < nUv; ++j) {
        if (!isPivot[j]) {
            tuvRows[j].emplace_back(freeIx[j], 1.0);
            ttRows[freeIx[j]].emplace_back(j, 1.0);
        } else {
            for (const auto& [c, w] : pivotExpr[j]) {
                tuvRows[j].emplace_back(freeIx[c], w);
                ttRows[freeIx[c]].emplace_back(j, w);
            }
        }
    }
    const accel::SparseMatrix Tuv = buildCsr(nUv, tuvRows);
    const accel::SparseMatrix Tt = buildCsr(W, ttRows);

    // L2 = blkdiag(L, L) over the UV block, from the unpinned cotan rows.
    accel::SparseMatrix L2;
    L2.rows = nUv;
    L2.rowStart.reserve(nUv + 1);
    L2.rowStart.push_back(0);
    for (std::size_t half = 0; half < 2; ++half) {
        for (std::size_t i = 0; i < nCut; ++i) {
            for (const auto& [j, w] : rows[i]) {
                L2.colIndex.push_back(half * nCut + j);
                L2.value.push_back(w);
            }
            L2.rowStart.push_back(L2.colIndex.size());
        }
    }

    // Reduced gradient gRed = Tt * g, g = [bu | bv].
    std::vector<float> g(nUv, 0.0f);
    for (std::size_t i = 0; i < nCut; ++i) {
        g[i] = bu[i];
        g[nCut + i] = bv[i];
    }
    accel::Buffer<float> gBuf(g), gRed;
    accel::spmv(backend, Tt, gBuf, gRed);
    const std::vector<float> gReduced(gRed.data(), gRed.data() + gRed.size());

    // Reduced operator A(p) = Tt * L2 * Tuv * p, applied via three spmv.
    accel::Buffer<float> tmpUv, tmpUv2, outBuf, pBuf;
    const auto applyA = [&](const std::vector<float>& p, std::vector<float>& out) {
        pBuf.upload(p);
        accel::spmv(backend, Tuv, pBuf, tmpUv);
        accel::spmv(backend, L2, tmpUv, tmpUv2);
        accel::spmv(backend, Tt, tmpUv2, outBuf);
        out.assign(outBuf.data(), outBuf.data() + outBuf.size());
    };

    // Masked Conjugate Gradient: solve A w = rhs over coordinates with mask==1 (others held at
    // their current w, folded into the effective RHS). Small ridge keeps A well-conditioned.
    std::vector<float> w(W, 0.0f);
    int totalCg = 0;
    const double ridge = 1e-8;
    int maskedSolveCalls = 0;
    const auto maskedSolve = [&](const std::vector<char>& mask) {
        ++maskedSolveCalls;
        // rhsEff = gReduced - A(wFixed), wFixed = w on !mask, 0 on mask.
        std::vector<float> wFixed(W, 0.0f);
        for (std::size_t i = 0; i < W; ++i) {
            if (!mask[i]) {
                wFixed[i] = w[i];
            }
        }
        std::vector<float> Aw;
        applyA(wFixed, Aw);
        std::vector<float> rhs(W);
        for (std::size_t i = 0; i < W; ++i) {
            rhs[i] = mask[i] ? (gReduced[i] - Aw[i]) : 0.0f;
        }
        // CG on the masked subspace, warm-started from the current w on mask coords (so each
        // greedy re-solve, which only perturbs one fixed integer, converges in a few iters).
        std::vector<float> x(W, 0.0f);
        for (std::size_t i = 0; i < W; ++i) {
            if (mask[i]) {
                x[i] = w[i];
            }
        }
        std::vector<float> Ax;
        applyA(x, Ax);  // x already lives on mask coords only
        std::vector<float> r(W), pv(W), Ap;
        double rs = 0.0;
        for (std::size_t i = 0; i < W; ++i) {
            if (mask[i]) {
                r[i] = rhs[i] - (Ax[i] + static_cast<float>(ridge) * x[i]);
                pv[i] = r[i];
                rs += static_cast<double>(r[i]) * r[i];
            } else {
                r[i] = 0.0f;
                pv[i] = 0.0f;
            }
        }
        const double rs0 = rs;
        if (rs0 > 0.0) {
            const int maxIt = static_cast<int>(W) + 1000;
            for (int it = 0; it < maxIt; ++it) {
                if (cancel != nullptr && (it & 63) == 0 && cancel->isCancelled()) {
                    break;
                }
                applyA(pv, Ap);
                double pAp = 0.0;
                for (std::size_t i = 0; i < W; ++i) {
                    if (mask[i]) {
                        Ap[i] += static_cast<float>(ridge) * pv[i];
                        pAp += static_cast<double>(pv[i]) * Ap[i];
                    } else {
                        Ap[i] = 0.0f;
                    }
                }
                ++totalCg;
                if (pAp <= 0.0) {
                    break;
                }
                const double alpha = rs / pAp;
                double rsNew = 0.0;
                for (std::size_t i = 0; i < W; ++i) {
                    if (!mask[i]) {
                        continue;
                    }
                    x[i] += static_cast<float>(alpha) * pv[i];
                    r[i] -= static_cast<float>(alpha * static_cast<double>(Ap[i]));
                    rsNew += static_cast<double>(r[i]) * r[i];
                }
                if (rsNew <= 1e-12 * rs0) {
                    break;
                }
                const double beta = rsNew / rs;
                for (std::size_t i = 0; i < W; ++i) {
                    if (mask[i]) {
                        pv[i] = r[i] + static_cast<float>(beta) * pv[i];
                    }
                }
                rs = rsNew;
            }
        }
        for (std::size_t i = 0; i < W; ++i) {
            if (mask[i]) {
                w[i] = x[i];
            }
        }
    };

    // Relaxed solve (all frees active), then BATCHED greedy integer rounding: each round pins
    // every still-free integer that is already confidently near an integer (|frac| <= kConfident)
    // and re-solves, so the continuous DOF absorb the rounding MIQ-style while the solve count
    // stays small (a few rounds, not one per integer). If a round pins nothing, fall back to the
    // single most-confident so it always terminates. Seamlessness holds for any integer values
    // (the reduction makes the free integers unconstrained); rounding only shapes distortion.
    std::vector<char> mask(W, 1);
    maskedSolve(mask);

    // MAGNITUDE CONTROL. The reduction leaves the independent integer translations
    // UNCONSTRAINED — any integers keep the map seamless — so a huge relaxed value would
    // round to a huge integer and blow the map up (the failure the divergence guard used to
    // reject). Bound each rounded translation to a physically sane range: a seamless integer
    // grid crosses at most O(grid extent) cells along any seam, so cap by the number of grid
    // lines the relaxed UV already spans (plus slack). Clamping here keeps seamlessness intact
    // and simply refuses to place a seam translation the geometry never justified.
    double uvSpan = 0.0;
    {
        accel::Buffer<float> wProbe(w), zProbe;
        accel::spmv(backend, Tuv, wProbe, zProbe);
        for (std::size_t i = 0; i < zProbe.size(); ++i) {
            uvSpan = std::max(uvSpan, static_cast<double>(std::abs(zProbe[i])));
        }
    }
    const double tCap = std::max(uvSpan * 2.0, std::sqrt(static_cast<double>(nCut))) + 8.0;
    const auto clampInt = [tCap](double val) {
        return std::clamp(std::round(val), -tCap, tCap);
    };

    std::vector<char> intPinned(intFree.size(), 0);
    constexpr double kConfident = 0.2;
    std::size_t remaining = intFree.size();
    while (remaining > 0) {
        if (cancel != nullptr && cancel->isCancelled()) {
            break;
        }
        // Pin every still-free integer that is already confidently near an integer.
        const auto pin = [&](std::size_t k) {
            const std::size_t ri = intFree[k];
            w[ri] = static_cast<float>(clampInt(static_cast<double>(w[ri])));
            mask[ri] = 0;
            intPinned[k] = 1;
            --remaining;
        };
        std::vector<std::pair<double, std::size_t>> frac;  // (|frac|, k) of the still-free ints
        std::size_t pinnedThisRound = 0;
        for (std::size_t k = 0; k < intFree.size(); ++k) {
            if (intPinned[k]) {
                continue;
            }
            const double val = static_cast<double>(w[intFree[k]]);
            const double f = std::abs(val - std::round(val));
            if (f <= kConfident) {
                pin(k);
                ++pinnedThisRound;
            } else {
                frac.emplace_back(f, k);
            }
        }
        // Nothing confident: pin a BATCH of the closest-to-integer frees, not just one, so the
        // solve count stays O(log) rounds instead of O(intFree). Seamlessness is unaffected (the
        // reduced integers are unconstrained); batching only rounds a few more before the solve
        // settles, which the next re-solve absorbs. Cuts the many single-pin tail rounds.
        if (pinnedThisRound == 0 && !frac.empty()) {
            const std::size_t batch = std::max<std::size_t>(1, frac.size() / 8);
            std::partial_sort(frac.begin(),
                              frac.begin() + static_cast<std::ptrdiff_t>(std::min(batch, frac.size())),
                              frac.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < batch && i < frac.size(); ++i) {
                pin(frac[i].second);
            }
        }
        maskedSolve(mask);
    }

    // Reconstruct z = T w on the UV block.
    accel::Buffer<float> wBuf(w), zUv;
    accel::spmv(backend, Tuv, wBuf, zUv);
    for (std::size_t i = 0; i < nCut; ++i) {
        u[i] = zUv[i];
        v[i] = zUv[nCut + i];
    }

    const bool dbg = std::getenv("CYBER_QC_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr,
                     "[qc] reduced: nCut=%zu seams=%zu vars=%zu free=%zu intFree=%zu maskedSolves=%d totalCg=%d\n",
                     nCut, nSeam, N, W, intFree.size(), maskedSolveCalls, totalCg);
    }
    return totalCg;
}

}  // namespace

Parameterization solveParameterization(const Mesh& mesh, const SeamlessSetup& setup,
                                       float spacing, accel::IBackend& backend,
                                       const CancelToken* cancel) {
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
    // from the combed target gradients Gu = e0/spacing, Gv = e1/spacing per face. Per-face
    // geometry (corner cut ids, gradient-of-hat vectors, area, the field frame e0/e1) is
    // cached so the divergence RHS can be re-assembled cheaply for the ARAP polish below
    // WITHOUT touching the Laplacian (only the target frame per face rotates).
    struct FaceData {
        std::array<std::size_t, 3> cut{};
        std::array<Vec3, 3> gradPhi{};
        float area = 0.0f;
        Vec3 e0{}, e1{};
    };
    std::vector<FaceData> faceData;
    faceData.reserve(mesh.faceCapacity());
    std::vector<std::unordered_map<std::size_t, float>> rows(nCut);
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
        FaceData fd;
        fd.area = 0.5f * area2;
        fd.e0 = normalized(rotQuarter(setup.field.direction(f), n, comb[fi]));
        fd.e1 = cross(n, fd.e0);
        for (int k = 0; k < 3; ++k) {
            fd.cut[static_cast<std::size_t>(k)] = cutOf(fi, vs[static_cast<std::size_t>(k)].value);
        }
        for (int k = 0; k < 3; ++k) {
            const Vec3 pk = p[static_cast<std::size_t>(k)];
            const Vec3 pa = p[static_cast<std::size_t>((k + 1) % 3)];
            const Vec3 pb = p[static_cast<std::size_t>((k + 2) % 3)];
            fd.gradPhi[static_cast<std::size_t>(k)] = cross(n, pb - pa) / area2;
            const float cotK = dot(pa - pk, pb - pk) / area2;
            const std::size_t ia = fd.cut[static_cast<std::size_t>((k + 1) % 3)];
            const std::size_t ib = fd.cut[static_cast<std::size_t>((k + 2) % 3)];
            const float w = 0.5f * cotK;
            rows[ia][ib] -= w;
            rows[ib][ia] -= w;
            rows[ia][ia] += w;
            rows[ib][ib] += w;
        }
        faceData.push_back(fd);
    }
    if (nCut == 0) {
        return out;
    }

    // Re-assemble the divergence RHS from per-face target frames. angle[f] rotates the field
    // frame (e0,e1) by theta before scaling to 1/spacing: target grad(u) = invS*R(theta)*e0,
    // grad(v) = invS*R(theta)*e1. theta==0 reproduces the plain field-aligned target.
    std::vector<float> bu(nCut, 0.0f), bv(nCut, 0.0f);
    const auto assembleRhs = [&](const std::vector<float>& angle) {
        std::fill(bu.begin(), bu.end(), 0.0f);
        std::fill(bv.begin(), bv.end(), 0.0f);
        for (std::size_t fdi = 0; fdi < faceData.size(); ++fdi) {
            const FaceData& fd = faceData[fdi];
            const float th = angle.empty() ? 0.0f : angle[fdi];
            const float ct = std::cos(th), st = std::sin(th);
            // R(theta) rows: (ct,-st) and (st,ct), applied to (e0,e1).
            const Vec3 gu = (fd.e0 * ct - fd.e1 * st) * invS;
            const Vec3 gv = (fd.e0 * st + fd.e1 * ct) * invS;
            for (int k = 0; k < 3; ++k) {
                const Vec3& gp = fd.gradPhi[static_cast<std::size_t>(k)];
                bu[fd.cut[static_cast<std::size_t>(k)]] += fd.area * dot(gp, gu);
                bv[fd.cut[static_cast<std::size_t>(k)]] += fd.area * dot(gp, gv);
            }
        }
    };
    assembleRhs({});

    // Pin ONE cut vertex PER CONNECTED COMPONENT of the (relaxed) Laplacian graph, so the
    // matrix is SPD for CG. With feature edges now cutting the surface into independent patches,
    // the cut-open mesh can have several components; a single global pin would leave the others
    // rank-deficient (constant nullspace) and the relaxed CG would drift arbitrarily far —
    // exactly the "map blows up" divergence. Each component gets its own pin at value 0; the
    // integer phase re-ties the components through their shared feature seams, so the per-patch
    // gauge freedom is absorbed by the seam translations.
    std::vector<std::size_t> comp(nCut, kInvalidIndex);
    {
        std::size_t nextComp = 0;
        for (std::size_t s = 0; s < nCut; ++s) {
            if (comp[s] != kInvalidIndex || rows[s].empty()) {
                continue;
            }
            std::queue<std::size_t> bfs;
            bfs.push(s);
            comp[s] = nextComp;
            while (!bfs.empty()) {
                const std::size_t cur = bfs.front();
                bfs.pop();
                for (const auto& [j, wj] : rows[cur]) {
                    if (j != cur && comp[j] == kInvalidIndex && !rows[j].empty()) {
                        comp[j] = nextComp;
                        bfs.push(j);
                    }
                }
            }
            ++nextComp;
        }
    }
    std::vector<char> isPin(nCut, 0);
    {
        std::vector<char> compPinned;
        for (std::size_t i = 0; i < nCut; ++i) {
            if (comp[i] == kInvalidIndex) {
                continue;
            }
            if (comp[i] >= compPinned.size()) {
                compPinned.resize(comp[i] + 1, 0);
            }
            if (!compPinned[comp[i]]) {
                compPinned[comp[i]] = 1;
                isPin[i] = 1;
            }
        }
    }

    // Pinned SPD operator A (identity row per pin/isolated vertex, reduced cotan rows
    // elsewhere). It depends only on the Laplacian, so it is built ONCE and reused across
    // every ARAP re-solve (only the RHS rotates).
    accel::SparseMatrix A;
    A.rows = nCut;
    A.rowStart.reserve(nCut + 1);
    A.rowStart.push_back(0);
    for (std::size_t i = 0; i < nCut; ++i) {
        if (isPin[i] || rows[i].empty()) {
            A.colIndex.push_back(i);
            A.value.push_back(1.0f);
            A.rowStart.push_back(A.colIndex.size());
            continue;
        }
        for (const auto& [j, w] : rows[i]) {
            if (isPin[j]) {
                continue;  // pinned to 0 -> no RHS contribution
            }
            A.colIndex.push_back(j);
            A.value.push_back(w);
        }
        A.rowStart.push_back(A.colIndex.size());
    }

    std::vector<float> u(nCut, 0.0f), v(nCut, 0.0f);
    const int maxIters = static_cast<int>(nCut) + 500;
    // Solve the relaxed (per-component pinned) Poisson for the current full RHS bu/bv. The
    // pin zeroes the RHS at the pinned/isolated vertices; u,v are warm-started from their
    // incoming value so each ARAP re-solve (a small RHS perturbation) converges fast.
    const auto relaxedSolve = [&]() {
        std::vector<float> pbu = bu, pbv = bv;
        for (std::size_t i = 0; i < nCut; ++i) {
            if (isPin[i] || rows[i].empty()) {
                pbu[i] = 0.0f;
                pbv[i] = 0.0f;
            }
        }
        out.cgIterationsU = conjugateGradient(backend, A, pbu, u, maxIters, 1e-8f, cancel);
        out.cgIterationsV = conjugateGradient(backend, A, pbv, v, maxIters, 1e-8f, cancel);
    };
    relaxedSolve();
    if (cancel != nullptr && cancel->isCancelled()) {
        return out;  // out.valid stays false -> caller degrades cleanly
    }

    // ARAP local-global polish (docs/native-miq-plan.md). The plain field-aligned target has
    // non-zero curl, so the Poisson projection introduces shear / non-uniform scale that makes
    // integer isolines cross the field obliquely and leaves non-quad caps at cones. Each round:
    //   LOCAL — per face, take the current UV Jacobian J in the (e0,e1) frame and find the
    //           closest rotation R(theta), theta = atan2(J_yx - J_xy, J_xx + J_yy);
    //   GLOBAL — re-assemble the divergence RHS with the target frame rotated by theta and
    //           re-solve the (unchanged) pinned Poisson.
    // Starting from theta==0 reproduces the field-aligned solve, so this only *relaxes* the map
    // toward a locally rigid grid; it cannot move singularities (those are fixed by the cut /
    // seam topology) and keeps the target frame norm at invS, so the map stays bounded. The
    // refined RHS then feeds the integer-seamless phase, which re-enforces exact seamlessness.
    //
    // GATED to closed (no open boundary) surfaces. A free boundary has no downstream safety net:
    // the isoline extractor's non-manifold/hole cleanup interacts badly with a boundary that the
    // polish has shifted, so a small ARAP perturbation there flips the extracted mesh
    // non-manifold — the one hard-constraint violation observed. A boundaried surface also
    // regresses under this polish (measured), so skipping it both preserves watertightness and
    // is the better-quality choice. The boundary fraction is ~0 for a closed (or numerically
    // cracked) surface and jumps as soon as a genuine perimeter appears.
    std::size_t nBoundaryEdge = 0, nAliveEdge = 0;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e)) {
            continue;
        }
        ++nAliveEdge;
        if (mesh.edgeFaceCount(e) == 1) {
            ++nBoundaryEdge;
        }
    }
    const double boundaryFrac =
        nAliveEdge == 0 ? 1.0
                        : static_cast<double>(nBoundaryEdge) / static_cast<double>(nAliveEdge);
    std::vector<float> angle(faceData.size(), 0.0f);
    constexpr int kArapIters = 6;
    constexpr float kMaxRot = kPi / 3.0f;  // ~60 deg cone: light insurance against sliver blow-ups
    constexpr double kBoundaryGate = 0.01;
    std::vector<float> raw(faceData.size(), 0.0f);
    for (int iter = 0; boundaryFrac < kBoundaryGate && iter < kArapIters; ++iter) {
        if (cancel != nullptr && cancel->isCancelled()) {
            return out;
        }
        double maxDelta = 0.0;
        for (std::size_t fdi = 0; fdi < faceData.size(); ++fdi) {
            const FaceData& fd = faceData[fdi];
            Vec3 gradU{0.0f, 0.0f, 0.0f}, gradV{0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 3; ++k) {
                const Vec3& gp = fd.gradPhi[static_cast<std::size_t>(k)];
                gradU = gradU + gp * u[fd.cut[static_cast<std::size_t>(k)]];
                gradV = gradV + gp * v[fd.cut[static_cast<std::size_t>(k)]];
            }
            const float jxx = dot(gradU, fd.e0), jxy = dot(gradU, fd.e1);
            const float jyx = dot(gradV, fd.e0), jyy = dot(gradV, fd.e1);
            // Closest-rotation angle, CLAMPED to a wide cone about the field frame. The clamp
            // only catches degenerate/sliver triangles whose noisy gradient would otherwise
            // rotate the target far enough to fold the map; well-shaped faces are unaffected.
            raw[fdi] = std::clamp(std::atan2(jyx - jxy, jxx + jyy), -kMaxRot, kMaxRot);
            maxDelta = std::max(maxDelta, static_cast<double>(std::abs(raw[fdi] - angle[fdi])));
        }
        angle.swap(raw);
        assembleRhs(angle);
        relaxedSolve();
        if (maxDelta < 1e-4) {
            break;  // rotations settled
        }
    }
    // Feed the ARAP-refined targets (field-aligned, unchanged, when the polish was skipped) into
    // the integer-seamless phase.
    assembleRhs(angle);
    const std::vector<float> bu0 = bu, bv0 = bv;
    if (cancel != nullptr && cancel->isCancelled()) {
        return out;
    }

    // Integer-seamless phase: collect the seam edges with the rotation rho taken from the
    // COMBED field (rho = comb[B] - comb[A] - periodJump, mod 4) — the exact grid symmetry the
    // Dirichlet energy aligns to, so the holonomy around every cone/junction is consistent (a
    // relaxed-UV estimate would drift at branch points). Then re-solve with the seam transitions
    // as hard constraints, reducing them to independent DOF and rounding the integer
    // translations (solveSeamlessReduced) — turning the non-rigid relaxed seam into a rigid
    // integer grid so seamlessUvResidual drops to ~0. Scales to hundreds of singularities.
    std::vector<SeamRef> seams;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || !setup.isCutEdge[ei]) {
            continue;
        }
        const auto ef = mesh.edgeFaces(e);
        if (mesh.faceSize(ef[0]) != 3 || mesh.faceSize(ef[1]) != 3) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        SeamRef s;
        s.aA = cutOf(ef[0].value, a.value);
        s.bA = cutOf(ef[0].value, b.value);
        s.aB = cutOf(ef[1].value, a.value);
        s.bB = cutOf(ef[1].value, b.value);
        // Combed grid symmetry from side A (ef[0]) to side B (ef[1]).
        const int p = setup.periodJump[ei];
        s.rho = (((comb[ef[1].value] - comb[ef[0].value] - p) % 4) + 4) % 4;
        seams.push_back(s);
    }
    // Gauges: one pin per connected component of the cut-open mesh (the isPin representatives
    // from the relaxed phase). The reduced integer solve needs every component's translation
    // nullspace pinned or it diverges. Empty components (all singular etc.) fall back to a
    // single default pin so the solve is always well-posed.
    std::vector<std::size_t> gauges;
    for (std::size_t i = 0; i < nCut; ++i) {
        if (isPin[i]) {
            gauges.push_back(i);
        }
    }
    if (gauges.empty()) {
        gauges.push_back(0);
    }
    // Sparse constraint-elimination MIQ: eliminates the seam rigidity + per-component gauges to
    // independent DOF and greedily rounds the integer translations. No dense dual, no seam cap —
    // it scales to hundreds of cones (spot: ~350 seam edges), reconciling branch-point holonomy.
    if (!seams.empty()) {
        solveSeamlessReduced(backend, nCut, rows, bu0, bv0, seams, gauges, u, v, cancel);
    }
    if (cancel != nullptr && cancel->isCancelled()) {
        return out;  // out.valid stays false
    }

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
