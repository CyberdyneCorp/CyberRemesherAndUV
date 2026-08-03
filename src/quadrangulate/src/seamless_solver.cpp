#include "cyber/quadrangulate/seamless_solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bimdf_quantize.hpp"
#include "cyber/accel/buffer.hpp"
#include "cyber/accel/primitives.hpp"
#include "cyber/core/math.hpp"
#include "sparse_cholesky.hpp"

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

// One growth step of the cut-tree: find the not-yet-connected singular vertex nearest to the
// in-tree set (plain BFS) and fill `parentEdge` with the search tree so the caller can walk
// the path back. Returns the target (invalid if none).
//
// MEASURED DEAD END (feature-pinning lever): routing this search as a 0/1 Dijkstra with
// feature edges cost 0, so tree paths ride creases and flat CAD patches keep no interior
// cuts, is unnecessary for CAD once the combed-target convention and the seam rho are fixed
// (box/cylinder are bit-identically perfect with plain BFS) and it REGRESSES organics whose
// coarse remesh carries many knife-edge feature chains (nefertiti lever-on singularities
// 80 -> 205: the tree snakes along wrinkle networks and shreds the map). Keep plain BFS.
VertexId nearestSingularBfs(const Mesh& mesh, const std::vector<char>& inTree,
                            const std::vector<char>& isSingular, std::vector<Index>& parentEdge) {
    std::vector<char> visited(mesh.vertexCapacity(), 0);
    std::queue<VertexId> q;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        if (inTree[vi]) {
            visited[vi] = 1;
            q.push(VertexId{vi});
        }
    }
    while (!q.empty()) {
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
                return w;
            }
            q.push(w);
        }
    }
    return VertexId{kInvalidIndex};
}

// --- Field-level dipole annihilation ---------------------------------------------------------
//
// buildSeamlessSetup historically promoted EVERY cross-field defect to a cone. On organics most
// of them are topologically-null noise: measured on the bunny, 211 singular vertices at
// totalIndex 59 — at least 72% are opposite-index (+1,-1) pairs clustered on high-curvature
// tubes, which the plain transport-averaging smoother cannot remove (dipole removal is a
// topological edit, not a smoothing one). This pass cancels close-by (+1,-1) pairs BEFORE cone
// promotion, the standard 4-RoSy dipole merge: edit the signed period jumps along a short
// connecting edge path (a unit jump change shifts the two endpoint indices by -+1 while interior
// path vertices are crossed twice with cancelling signs), then re-relax the face angles with the
// jumps FROZEN so the field itself absorbs the 90-degree residual. Poincare-Hopf is preserved by
// construction (+1 and -1 annihilate) and re-verified from the actual field; feature-pinned
// faces are never touched and paths never cross feature edges, so the feature binding
// (combedDirection / rho / periodJump semantics) is intact — everything downstream is recomputed
// from the modified field. Kill switch: CYBER_QC_NO_CONE_MERGE. Pairing radius (edge hops on the
// isotropic remesh, i.e. ~targetEdgeLength units): CYBER_QC_CONE_MERGE_RADIUS, default 3.
//
// This is NOT a retry of the exhausted single-level field-smoothing levers (ROADMAP c7): those
// tuned the smoother's energy; this edits the field's singularity topology directly.

// Signed period jump of edge e for the canonical face order (ef[0] -> ef[1]) given per-face
// unwrapped angles theta: D = (theta[ef1] - phi1) - (theta[ef0] - phi0), q = round(D / 90deg).
// phi is the edge angle in each face's frame; the edge's a->b direction sign cancels in D.
float jumpDelta(const Mesh& mesh, const CrossField& field, const std::vector<float>& theta,
                EdgeId e) {
    const auto ef = mesh.edgeFaces(e);
    const auto [a, b] = mesh.edgeVertices(e);
    const Vec3 d = normalized(mesh.position(b) - mesh.position(a));
    const float phi0 = frameAngle(d, field.tangent[ef[0].value], field.bitangent[ef[0].value]);
    const float phi1 = frameAngle(d, field.tangent[ef[1].value], field.bitangent[ef[1].value]);
    return (theta[ef[1].value] - phi1) - (theta[ef[0].value] - phi0);
}

// Cross-field index at vertex v computed from frozen integer jumps q instead of per-crossing
// wrapping: contribution of crossing f -> fNext across a spoke is s*(D - q*90deg) with s = -1
// when f is the canonical ef[0]. Identical to vertexIndex while q == round(D/90deg) (its
// initialisation), and responds to jump edits, which per-crossing wrapping cannot. Returns
// INT_MIN when the ring cannot be walked or a spoke has no jump entry (caller must skip).
int vertexIndexFromJumps(const Mesh& mesh, const CrossField& field, const std::vector<float>& theta,
                         const std::vector<int>& jump, const std::vector<char>& hasJump,
                         VertexId v) {
    constexpr int kNoIndex = std::numeric_limits<int>::min();
    const std::size_t valence = mesh.vertexFaces(v).size();
    if (valence < 3) {
        return kNoIndex;
    }
    const FaceId start = mesh.vertexFaces(v)[0];
    float residual = 0.0f;
    float angleSum = 0.0f;
    FaceId f = start;
    for (std::size_t step = 0; step < valence; ++step) {
        angleSum += interiorAngle(mesh, f, v);
        const VertexId w = ccwNext(mesh, f, v);
        if (w.value == kInvalidIndex) {
            return kNoIndex;
        }
        const EdgeId spoke = mesh.edgeBetween(v, w);
        if (!spoke.valid() || mesh.edgeFaceCount(spoke) != 2 || !hasJump[spoke.value]) {
            return kNoIndex;
        }
        const auto ef = mesh.edgeFaces(spoke);
        const FaceId fNext = (ef[0] == f) ? ef[1] : ef[0];
        const float d =
            jumpDelta(mesh, field, theta, spoke) - static_cast<float>(jump[spoke.value]) * kHalfPi;
        residual += (ef[0] == f) ? -d : d;
        f = fNext;
    }
    if (f != start) {
        return kNoIndex;
    }
    const float defect = 2.0f * kPi - angleSum;
    return static_cast<int>(std::lround((residual + defect) / kHalfPi));
}

// Faces the cross-field smoother held constrained: touching a feature or boundary edge, or a
// crease-aligned interior edge (computeCrossField's pin set, planarity gate dropped — a
// superset is safe here since pinned faces are merely excluded from editing/relaxation).
std::vector<char> fieldPinnedFaces(const Mesh& mesh) {
    float alignDeg = 45.0f;
    if (const char* fc = std::getenv("CYBER_QC_FIELD_CREASE_DEG"); fc != nullptr) {
        alignDeg = static_cast<float>(std::atof(fc));
    }
    const float alignCos = alignDeg > 0.0f ? std::cos(alignDeg * kPi / 180.0f) : 2.0f;
    std::vector<char> pinned(mesh.faceCapacity(), 0);
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        for (std::size_t k = 0; k < fv.size() && !pinned[fi]; ++k) {
            const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
            if (!e.valid()) {
                continue;
            }
            if (mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
                pinned[fi] = 1;
                continue;
            }
            if (alignCos <= 1.0f) {
                const auto ef = mesh.edgeFaces(e);
                if (dot(normalized(mesh.faceNormal(ef[0])), normalized(mesh.faceNormal(ef[1]))) <
                    alignCos) {
                    pinned[fi] = 1;
                }
            }
        }
    }
    return pinned;
}

// Cancels close-by (+1,-1) cross-field index pairs in place. See the block comment above.
void annihilateFieldDipoles(const Mesh& mesh, CrossField& field) {
    if (std::getenv("CYBER_QC_NO_CONE_MERGE") != nullptr) {
        return;
    }
    // Corpus-calibrated pairing radius, in edge hops on the isotropic remesh (~targetEdgeLength
    // units). 5 beat 3 and 8 on the organic corpus with every guard in place (spot's angle cost
    // in particular is LOWER at 5 than 3: 6.72 vs 7.22 deg against a 6.43 baseline); nefertiti
    // saturates at 8 (r12/r16 byte-identical — every remaining pair is pinned-web-blocked or
    // guard-rejected).
    int radius = 5;
    if (const char* r = std::getenv("CYBER_QC_CONE_MERGE_RADIUS"); r != nullptr) {
        radius = std::clamp(std::atoi(r), 1, 16);
    }
    constexpr int kRegionRings = 6;     // relax neighbourhood around an edited path
    constexpr int kRelaxSweeps = 400;   // frozen-jump Gauss-Seidel cap
    constexpr float kRelaxTol = 1e-5f;  // radians; max per-sweep angle change

    // Per-vertex field indices; the +1/-1 cone lists.
    std::vector<int> kOrig(mesh.vertexCapacity(), 0);
    std::vector<VertexId> plusCones;
    std::size_t minusCount = 0;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v)) {
            continue;
        }
        kOrig[vi] = vertexIndex(mesh, field, v);
        if (kOrig[vi] == 1) {
            plusCones.push_back(v);
        } else if (kOrig[vi] == -1) {
            ++minusCount;
        }
    }
    if (plusCones.empty() || minusCount == 0) {
        return;
    }

    const std::vector<char> pinned = fieldPinnedFaces(mesh);

    // An edge the cancellation path (and the jump system) may use: interior, non-feature, and
    // not bordering a pinned face — pinned faces cannot relax, so a 90-degree residual injected
    // next to them cannot diffuse and would concentrate into a shear band.
    // CURVATURE GUARD (bench-caught on the torus, second failure mode after the topology
    // guard: sing 64 -> 75, angle 20.4 -> 26.1 from DISK-region cancellations alone): a cone
    // pair sitting in strongly STRUCTURED curvature is demanded by the geometry — the field
    // is smoother without it, but the parameterization must then stretch across the curvature
    // and cell quality collapses (torus: cones ring the inner/outer equators). The measured
    // discriminator is the 2-ring defect CONSISTENCY ratio |sum|/sum-of-abs: smooth structured
    // curvature has every vertex curving the same way (torus pairs r ~0.99), while the
    // cancellable wrinkle-noise dipoles live in alternating-sign curvature (armadillo median
    // r 0.36; the rule rejects 11/13 torus pairs and only 4% of armadillo's).
    // Thresholds: |2-ring defect| > 10 degrees at consistency > 0.6 rejects 13/13 torus pairs
    // (0.8 left two at r0.65/0.68 whose cancellation alone cost the torus +28 sing / +7deg
    // angle) and 18% of armadillo's.
    const float curvThreshold = kPi / 18.0f;
    const float kStructureRatio = 0.6f;
    // Returns {signed defect sum, absolute defect sum} over v's 2-ring: the ratio |sum|/abs
    // separates smooth structured curvature (ratio ~1: every vertex curves the same way, the
    // cone is geometry-demanded) from wrinkle noise (alternating signs, ratio ~0).
    const auto ringDefect = [&](VertexId v) {
        float sum = 0.0f;
        float absSum = 0.0f;
        std::vector<VertexId> ring{v};
        std::set<Index> vis{v.value};
        std::size_t head = 0;
        for (int depth = 0; depth < 2; ++depth) {
            const std::size_t tail = ring.size();
            for (; head < tail; ++head) {
                for (const EdgeId e : mesh.vertexEdges(ring[head])) {
                    if (!mesh.isAlive(e)) {
                        continue;
                    }
                    const auto [a, b] = mesh.edgeVertices(e);
                    const VertexId w = (a == ring[head]) ? b : a;
                    if (vis.insert(w.value).second) {
                        ring.push_back(w);
                    }
                }
            }
        }
        for (const VertexId u : ring) {
            float angleSum = 0.0f;
            bool interior = true;
            for (const FaceId f : mesh.vertexFaces(u)) {
                if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
                    interior = false;
                    break;
                }
                angleSum += interiorAngle(mesh, f, u);
            }
            for (const EdgeId e : mesh.vertexEdges(u)) {
                if (mesh.isAlive(e) && mesh.edgeFaceCount(e) != 2) {
                    interior = false;  // boundary vertex: defect undefined, skip
                    break;
                }
            }
            if (interior) {
                const float defect = 2.0f * kPi - angleSum;
                sum += defect;
                absSum += std::abs(defect);
            }
        }
        return std::make_pair(sum, absSum);
    };

    // MEASURED DEAD END: allowing pinned-adjacent path edges (nefertiti r5: pairsTried
    // 199 -> 866 but cancelled 171 -> 172, rejectedRelax 22 -> 632) — the injected 90-degree
    // residual cannot diffuse past pinned faces, exactly the shear-band failure the verify
    // predicts, so nearly every extra pair reverts. Keep paths off pinned borders.
    const auto usableEdge = [&](EdgeId e) {
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || mesh.isFeatureEdge(e)) {
            return false;
        }
        const auto ef = mesh.edgeFaces(e);
        return pinned[ef[0].value] == 0 && pinned[ef[1].value] == 0;
    };

    // Reusable full-size scratch (meshes here are the coarse isotropic remesh, ~1e4 faces).
    std::vector<float> theta(mesh.faceCapacity(), 0.0f);
    std::vector<int> jump(mesh.edgeCapacity(), 0);
    std::vector<char> hasJump(mesh.edgeCapacity(), 0);
    std::vector<int> vertexDepth(mesh.vertexCapacity(), -1);
    std::vector<char> inRegion(mesh.faceCapacity(), 0);
    std::vector<char> matched(mesh.vertexCapacity(), 0);
    std::vector<Index> parent(mesh.vertexCapacity(), kInvalidIndex);
    std::vector<char> vertexSeen(mesh.vertexCapacity(), 0);
    std::vector<char> edgeSeen(mesh.edgeCapacity(), 0);

    int pairsTried = 0, cancelled = 0, rejectedEdit = 0, rejectedRelax = 0, rejectedTopo = 0,
        rejectedCurv = 0;

    // Progressive matching: tight pairs first (they are the least ambiguous field noise), the
    // pairing radius widening per inner round; failed pairs are retried in later outer rounds
    // because a neighbouring cancellation changes the local field they were rejected on.
    constexpr int kMaxRounds = 4;
    std::set<std::pair<Index, Index>> failedPairs;
    const auto cancelOnePair = [&](VertexId v0, int depthLimit) {
        // BFS over usable edges to the nearest unmatched -1 cone within depthLimit hops.
        std::fill(parent.begin(), parent.end(), kInvalidIndex);
        std::vector<char> visited(mesh.vertexCapacity(), 0);
        std::queue<std::pair<VertexId, int>> bfs;
        bfs.emplace(v0, 0);
        visited[v0.value] = 1;
        VertexId vEnd{kInvalidIndex};
        while (!bfs.empty() && vEnd.value == kInvalidIndex) {
            const auto [u, depth] = bfs.front();
            bfs.pop();
            if (depth >= depthLimit) {
                continue;
            }
            for (const EdgeId e : mesh.vertexEdges(u)) {
                if (!usableEdge(e)) {
                    continue;
                }
                const auto [a, b] = mesh.edgeVertices(e);
                const VertexId w = (a == u) ? b : a;
                if (visited[w.value]) {
                    continue;
                }
                visited[w.value] = 1;
                parent[w.value] = e.value;
                if (kOrig[w.value] == -1 && !matched[w.value]) {
                    vEnd = w;
                    break;
                }
                bfs.emplace(w, depth + 1);
            }
        }
        if (vEnd.value == kInvalidIndex || !failedPairs.emplace(v0.value, vEnd.value).second) {
            return false;  // no partner in range, or this exact pair already failed
        }
        const auto [defectPlus, absPlus] = ringDefect(v0);
        const auto [defectMinus, absMinus] = ringDefect(vEnd);
        const float ratioPlus = absPlus > 1e-6f ? std::abs(defectPlus) / absPlus : 0.0f;
        const float ratioMinus = absMinus > 1e-6f ? std::abs(defectMinus) / absMinus : 0.0f;
        if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
            std::fprintf(
                stderr, "[qc] dipole pair defect: +%.1fdeg(r%.2f) -%.1fdeg(r%.2f)\n",
                static_cast<double>(defectPlus * 180.0f / kPi), static_cast<double>(ratioPlus),
                static_cast<double>(defectMinus * 180.0f / kPi), static_cast<double>(ratioMinus));
        }
        if ((ratioPlus > kStructureRatio && std::abs(defectPlus) > curvThreshold) ||
            (ratioMinus > kStructureRatio && std::abs(defectMinus) > curvThreshold)) {
            ++rejectedCurv;  // curvature-demanded pair (see the guard comment above)
            return false;
        }
        // Path vertices vEnd -> v0, then reversed to run v0 -> vEnd.
        std::vector<VertexId> path{vEnd};
        while (path.back() != v0) {
            const EdgeId e{parent[path.back().value]};
            const auto [a, b] = mesh.edgeVertices(e);
            path.push_back((a == path.back()) ? b : a);
        }
        std::reverse(path.begin(), path.end());
        ++pairsTried;
        bool ok = false;

        // Region: vertices within `rings` of the path (never across a feature edge), and every
        // alive triangle incident to them. Free faces (relaxed + written back) keep a >=1-face
        // fixed rim: all their vertices sit at depth <= rings - 1, so every vertex whose ring
        // can change is inside the visited set and gets re-verified.
        //
        // DISK-TOPOLOGY GUARD (bench-caught on the torus: sing 64 -> 140, angle 20.4 -> 30.7
        // with a FIELD that got cleaner, 32 -> 6 cones): the per-vertex index verification
        // below is complete exactly when the free faces live in a topological disk — every
        // closed loop through relaxed faces then has its holonomy fixed by the verified
        // enclosed indices, so NO holonomy can change anywhere. If the region wraps a handle
        // or tube (annulus), the wrap generator's rotational holonomy is unchecked and a jump
        // edit can twist the field ~90 degrees around it with every vertex index intact (and,
        // on a fat tube, almost no smoothness-energy cost); the parameterization then spirals
        // and extraction shreds. A global genus gate is NOT the answer: organic work meshes
        // carry accidental bridge handles (armadillo: 56 non-manifold pinch edges and a
        // genus-positive excised complex) yet their tube-region cancellations measurably help.
        // Instead require the region itself to be a disk, retrying with a halved radius when
        // the full region wraps (a capped-tube wrap often un-wraps at smaller radius).
        std::vector<VertexId> regionVerts;
        std::vector<FaceId> regionFaces;
        int regionRings = 0;
        for (const int rings : {kRegionRings, kRegionRings / 2, kRegionRings / 3}) {
            std::queue<VertexId> rq;
            for (const VertexId pv : path) {
                vertexDepth[pv.value] = 0;
                rq.push(pv);
                regionVerts.push_back(pv);
            }
            while (!rq.empty()) {
                const VertexId u = rq.front();
                rq.pop();
                if (vertexDepth[u.value] >= rings) {
                    continue;
                }
                // Region growth DOES cross feature edges (the path never may): faces beyond a
                // crease are pinned and stay frozen, but excluding them punches holes into the
                // region wherever a feature web encircles it — nefertiti's 1805 feature edges
                // made 114 of 211 regions spuriously non-disk and the guard rejected them.
                for (const EdgeId e : mesh.vertexEdges(u)) {
                    if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2) {
                        continue;
                    }
                    const auto [a, b] = mesh.edgeVertices(e);
                    const VertexId w = (a == u) ? b : a;
                    if (vertexDepth[w.value] >= 0) {
                        continue;
                    }
                    vertexDepth[w.value] = vertexDepth[u.value] + 1;
                    regionVerts.push_back(w);
                    rq.push(w);
                }
            }
            for (const VertexId u : regionVerts) {
                for (const FaceId f : mesh.vertexFaces(u)) {
                    if (mesh.isAlive(f) && mesh.faceSize(f) == 3 && !inRegion[f.value]) {
                        inRegion[f.value] = 1;
                        regionFaces.push_back(f);
                    }
                }
            }
            // Euler characteristic of the region complex: disk chi=1 (chi=2 would be the whole
            // closed surface, where vertex loops also generate every cycle).
            long vCount = 0, eCount = 0;
            for (const FaceId f : regionFaces) {
                const std::vector<VertexId> fv = mesh.faceVertices(f);
                for (std::size_t k = 0; k < fv.size(); ++k) {
                    if (!vertexSeen[fv[k].value]) {
                        vertexSeen[fv[k].value] = 1;
                        ++vCount;
                    }
                    const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
                    if (e.valid() && !edgeSeen[e.value]) {
                        edgeSeen[e.value] = 1;
                        ++eCount;
                    }
                }
            }
            for (const FaceId f : regionFaces) {
                const std::vector<VertexId> fv = mesh.faceVertices(f);
                for (std::size_t k = 0; k < fv.size(); ++k) {
                    vertexSeen[fv[k].value] = 0;
                    const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
                    if (e.valid()) {
                        edgeSeen[e.value] = 0;
                    }
                }
            }
            const long chi = vCount - eCount + static_cast<long>(regionFaces.size());
            if (chi == 1 || chi == 2) {
                regionRings = rings;
                break;
            }
            for (const VertexId u : regionVerts) {
                vertexDepth[u.value] = -1;
            }
            for (const FaceId f : regionFaces) {
                inRegion[f.value] = 0;
            }
            regionVerts.clear();
            regionFaces.clear();
        }
        if (regionRings == 0) {
            ++rejectedTopo;
            return false;
        }

        // Local jump system over the region: unwrapped per-face angles seeded from the field's
        // representative, canonical signed jumps from rounding (so initially the jump-based
        // index reproduces vertexIndex exactly).
        for (const FaceId f : regionFaces) {
            theta[f.value] = field.angle(f);
        }
        // Feature edges DO get a jump entry: the index verifier must be able to walk rings whose
        // spokes include a crease (their adjacent faces are pinned, so the relax never moves
        // them and the path never uses them — the entry is read-only truth, never edited).
        std::vector<EdgeId> regionEdges;
        for (const FaceId f : regionFaces) {
            const std::vector<VertexId> fv = mesh.faceVertices(f);
            for (std::size_t k = 0; k < fv.size(); ++k) {
                const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
                if (!e.valid() || hasJump[e.value] || !mesh.isAlive(e) ||
                    mesh.edgeFaceCount(e) != 2) {
                    continue;
                }
                const auto ef = mesh.edgeFaces(e);
                if (!inRegion[ef[0].value] || !inRegion[ef[1].value]) {
                    continue;
                }
                jump[e.value] =
                    static_cast<int>(std::lround(jumpDelta(mesh, field, theta, e) / kHalfPi));
                hasJump[e.value] = 1;
                regionEdges.push_back(e);
            }
        }

        const auto kj = [&](VertexId v) {
            return vertexIndexFromJumps(mesh, field, theta, jump, hasJump, v);
        };

        // Edit the signed jumps along the path, one edge at a time; each edge's sign is fixed
        // by requiring the trailing vertex to land on its target index (0 for the +1 cone,
        // unchanged for interiors). Any failure reverts the whole path.
        bool editOk = true;
        std::vector<std::pair<Index, int>> edits;
        for (std::size_t i = 0; i + 1 < path.size() && editOk; ++i) {
            const EdgeId e = mesh.edgeBetween(path[i], path[i + 1]);
            if (!e.valid() || !hasJump[e.value]) {
                editOk = false;
                break;
            }
            const int want = (i == 0) ? 0 : kOrig[path[i].value];
            editOk = false;
            for (const int dq : {1, -1}) {
                jump[e.value] += dq;
                if (kj(path[i]) == want) {
                    edits.emplace_back(e.value, dq);
                    editOk = true;
                    break;
                }
                jump[e.value] -= dq;
            }
        }
        if (editOk && kj(path.back()) != 0) {
            editOk = false;
        }
        if (!editOk) {
            for (const auto& [ei, dq] : edits) {
                jump[ei] -= dq;
            }
            ++rejectedEdit;
        } else {
            // Frozen-jump Gauss-Seidel: relax the unwrapped angles so the field absorbs the
            // edited topology. Free faces keep a fixed rim (see region comment) and are never
            // pinned; the jumps bind because theta is a real variable, not a wrapped one.
            std::vector<FaceId> freeFaces;
            for (const FaceId f : regionFaces) {
                if (pinned[f.value]) {
                    continue;
                }
                const std::vector<VertexId> fv = mesh.faceVertices(f);
                bool interior = true;
                for (const VertexId u : fv) {
                    if (vertexDepth[u.value] < 0 || vertexDepth[u.value] > regionRings - 1) {
                        interior = false;
                        break;
                    }
                }
                if (interior) {
                    freeFaces.push_back(f);
                }
            }
            for (int sweep = 0; sweep < kRelaxSweeps; ++sweep) {
                float maxDelta = 0.0f;
                for (const FaceId f : freeFaces) {
                    float acc = 0.0f;
                    int cnt = 0;
                    const std::vector<VertexId> fv = mesh.faceVertices(f);
                    for (std::size_t k = 0; k < fv.size(); ++k) {
                        const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
                        if (!e.valid() || !hasJump[e.value]) {
                            continue;
                        }
                        const auto ef = mesh.edgeFaces(e);
                        const FaceId g = (ef[0] == f) ? ef[1] : ef[0];
                        const auto [a, b] = mesh.edgeVertices(e);
                        const Vec3 d = normalized(mesh.position(b) - mesh.position(a));
                        const float phiF =
                            frameAngle(d, field.tangent[f.value], field.bitangent[f.value]);
                        const float phiG =
                            frameAngle(d, field.tangent[g.value], field.bitangent[g.value]);
                        const float q = static_cast<float>(jump[e.value]) * kHalfPi;
                        // Constraint (theta_ef1 - phi1) - (theta_ef0 - phi0) = q, solved for f.
                        acc += (ef[0] == f) ? theta[g.value] - phiG + phiF - q
                                            : theta[g.value] - phiG + phiF + q;
                        ++cnt;
                    }
                    if (cnt == 0) {
                        continue;
                    }
                    const float target = acc / static_cast<float>(cnt);
                    const float delta = target - theta[f.value];
                    theta[f.value] += 0.8f * delta;
                    maxDelta = std::max(maxDelta, std::abs(delta));
                }
                if (maxDelta < kRelaxTol) {
                    break;
                }
            }
            // Write back and verify against the REAL field-walk index: the pair must be gone
            // and no other vertex in the region may change. Pin blockage and sign bugs surface
            // here and revert cleanly. (A strict "region smoothness energy must not increase"
            // guard was measured too aggressive here: it rejected 72 of armadillo's 169 pairs
            // whose acceptance had already been shown metric-clean, and it caught neither
            // torus failure mode — the disk-topology and curvature guards above are the ones
            // that actually protect the bench.)
            std::vector<std::pair<Index, std::pair<float, float>>> snapshot;
            snapshot.reserve(freeFaces.size());
            for (const FaceId f : freeFaces) {
                snapshot.emplace_back(f.value,
                                      std::make_pair(field.real[f.value], field.imag[f.value]));
                field.real[f.value] = std::cos(4.0f * theta[f.value]);
                field.imag[f.value] = std::sin(4.0f * theta[f.value]);
            }
            bool verifyOk = true;
            for (const VertexId u : regionVerts) {
                const int want = (u == v0 || u == vEnd) ? 0 : kOrig[u.value];
                if (vertexIndex(mesh, field, u) != want) {
                    verifyOk = false;
                    break;
                }
            }
            if (!verifyOk) {
                for (const auto& [fi, ri] : snapshot) {
                    field.real[fi] = ri.first;
                    field.imag[fi] = ri.second;
                }
                for (const auto& [ei, dq] : edits) {
                    jump[ei] -= dq;
                }
                ++rejectedRelax;
            } else {
                kOrig[v0.value] = 0;
                kOrig[vEnd.value] = 0;
                matched[v0.value] = 1;
                matched[vEnd.value] = 1;
                ++cancelled;
                ok = true;
            }
        }

        // Reset the per-pair scratch.
        for (const VertexId u : regionVerts) {
            vertexDepth[u.value] = -1;
        }
        for (const FaceId f : regionFaces) {
            inRegion[f.value] = 0;
        }
        for (const EdgeId e : regionEdges) {
            hasJump[e.value] = 0;
        }
        return ok;
    };

    for (int round = 0; round < kMaxRounds; ++round) {
        bool progress = false;
        for (int d = 1; d <= radius; ++d) {
            for (const VertexId v0 : plusCones) {
                if (!matched[v0.value] && cancelOnePair(v0, d)) {
                    progress = true;
                }
            }
        }
        if (!progress) {
            break;
        }
        failedPairs.clear();  // neighbouring cancellations changed the field: retry rejects
    }

    if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[qc] dipole merge: +cones=%zu pairsTried=%d cancelled=%d "
                     "rejectedEdit=%d rejectedRelax=%d rejectedTopo=%d rejectedCurv=%d\n",
                     plusCones.size(), pairsTried, cancelled, rejectedEdit, rejectedRelax,
                     rejectedTopo, rejectedCurv);
    }
}

}  // namespace

std::size_t SeamlessSetup::singularityCount() const {
    return static_cast<std::size_t>(std::count_if(singularityIndex.begin(), singularityIndex.end(),
                                                  [](int k) { return k != 0; }));
}

int SeamlessSetup::totalIndex() const {
    return std::accumulate(singularityIndex.begin(), singularityIndex.end(), 0);
}

SeamlessSetup buildSeamlessSetup(const Mesh& mesh, int iterations, accel::IBackend& backend,
                                 bool featureBinding) {
    SeamlessSetup setup;
    if (mesh.faceCapacity() == 0) {
        return setup;
    }
    // Experimental: derive the cross field from the multiresolution per-vertex
    // orientation (coarse-to-fine singularity placement) instead of single-level
    // face smoothing. Gated so the stock seamless path is unchanged.
    if (std::getenv("CYBER_QC_CROSSFIELD_MULTIRES") != nullptr) {
        setup.field = computeCrossFieldFromOrientation(mesh, iterations, backend);
    } else {
        setup.field = computeCrossField(mesh, iterations, backend);
    }

    // Cancel topologically-null (+1,-1) field cone pairs before they are promoted to cones
    // below (kill switch CYBER_QC_NO_CONE_MERGE; see annihilateFieldDipoles).
    annihilateFieldDipoles(mesh, setup.field);

    // Per-edge period jumps. Historically skipped for feature edges (they are always cut, so
    // the comb never crosses them) — but the seam transition rho DOES need the intrinsic field
    // jump across a crease: without it every feature seam's rho is off by the crease's
    // representative wrap and the reduced integer phase fights the (correct) relaxed targets.
    // Computed for feature edges too under the feature-pinning lever; lever-off keeps the
    // historical skip bit-compatible.
    setup.featureBinding = featureBinding;
    const bool featureLever = featureBinding;
    setup.periodJump.assign(mesh.edgeCapacity(), 0);
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (mesh.isAlive(e) && mesh.edgeFaceCount(e) == 2 &&
            (featureLever || !mesh.isFeatureEdge(e))) {
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
            // Search from all in-tree vertices to the nearest not-yet-connected singular one.
            std::vector<Index> parentEdge(mesh.vertexCapacity(), kInvalidIndex);
            const VertexId target = nearestSingularBfs(mesh, inTree, isSingular, parentEdge);
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

// Cache for the CYBER_QC_DIRECT sparse-direct solve path (see the header).
// Both cached operators are provably attempt-invariant: the pinned Poisson A
// depends only on the cut Laplacian + pins, and the reduced operator
// M = Tt*L2*Tuv only on the seam/constraint structure — the calibration
// spacing enters the RHS alone (assembleRhs scales by 1/spacing).
class SeamlessSolveCacheImpl {
public:
    // Phase 1: pinned Poisson operator (relaxed + ARAP re-solves).
    bool aReady = false;
    SparseCholesky cholA;

    // Phase 2: reduced integer-phase operator + the dense integer block of its
    // inverse (the Woodbury bordered solves of the greedy rounding).
    bool mReady = false;
    std::size_t mW = 0;
    std::vector<std::size_t> mIntFree;  // reuse guard: structure must match exactly
    SparseCholesky cholM;
    std::vector<double> mD;  // |intFree|^2 dense block of (M + ridge)^-1
};

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
                      const std::vector<float>& b, std::vector<float>& x, int maxIters, float tol,
                      const CancelToken* cancel = nullptr) {
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

long msSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count());
}

// Ridge added to the reduced operator so it stays SPD (matches the masked CG,
// which applies the same ridge inside every solve).
constexpr double kReducedRidge = 1e-8;

// Build the explicit reduced operator M = Tt * blkdiag(L, L) * Tuv in double
// CSR (sorted columns). `rows` is the cut cotan Laplacian; tuvRows/ttRows are
// the Gauss-Jordan reduction map and its transpose as (column, weight) lists.
// Shared by the direct factorization (CYBER_QC_DIRECT) and the fused-operator
// CG fallback (CYBER_QC_FUSED_OPERATOR).
void buildReducedOperator(std::size_t nCut,
                          const std::vector<std::unordered_map<std::size_t, float>>& rows,
                          const std::vector<std::vector<std::pair<std::size_t, double>>>& tuvRows,
                          const std::vector<std::vector<std::pair<std::size_t, double>>>& ttRows,
                          std::size_t W, std::vector<std::size_t>& mStart,
                          std::vector<std::size_t>& mCol, std::vector<double>& mVal) {
    mStart.assign(W + 1, 0);
    mCol.clear();
    mVal.clear();
    std::vector<double> acc(W, 0.0);
    std::vector<std::size_t> markRow(W, kInvalidIndex);
    std::vector<std::size_t> touched;
    for (std::size_t r = 0; r < W; ++r) {
        touched.clear();
        for (const auto& [iUv, wi] : ttRows[r]) {
            const std::size_t offset = iUv < nCut ? 0 : nCut;
            for (const auto& [j, lw] : rows[iUv - offset]) {
                const double c = wi * static_cast<double>(lw);
                for (const auto& [cIdx, tw] : tuvRows[offset + j]) {
                    if (markRow[cIdx] != r) {
                        markRow[cIdx] = r;
                        acc[cIdx] = 0.0;
                        touched.push_back(cIdx);
                    }
                    acc[cIdx] += c * tw;
                }
            }
        }
        std::sort(touched.begin(), touched.end());
        for (const std::size_t cIdx : touched) {
            mCol.push_back(cIdx);
            mVal.push_back(acc[cIdx]);
        }
        mStart[r + 1] = mCol.size();
    }
}

// Exact direct engine for the reduced integer phase (CYBER_QC_DIRECT). One
// sparse factorization of M + ridge replaces every masked CG:
//   relaxed solve       -> x0 = (M+ridge)^-1 g (a back-substitution);
//   each rounding round -> the bordered KKT system over the pinned set S
//     (E selects S):  D_SS lambda = x0_S - c,  w = x0 - (M+ridge)^-1 E^T lambda
//     with D = ((M+ridge)^-1)_{int,int} precomputed once (dense symmetric; the
//     greedy loop only ever READS w at integer coordinates, so per-round work
//     is dense and tiny — the pinned set only grows, hence the incremental
//     dense Cholesky).
// Every w this exposes is EXACTLY maskedSolve's fixed point
// ((M+ridge)_FF w_F = g_F - M_FS c, w_S = c), so the greedy pin SCHEDULE runs
// byte-for-byte the same code; only the linear solver under it changes.
class DirectRounding {
public:
    bool init(accel::IBackend& backend, std::size_t nCut,
              const std::vector<std::unordered_map<std::size_t, float>>& rows,
              const std::vector<std::vector<std::pair<std::size_t, double>>>& tuvRows,
              const std::vector<std::vector<std::pair<std::size_t, double>>>& ttRows,
              const std::vector<float>& gReduced, const std::vector<std::size_t>& intFree,
              std::size_t W, SeamlessSolveCacheImpl* cache, const CancelToken* cancel) {
        m_W = W;
        m_intFree = &intFree;
        const std::size_t nInt = intFree.size();
        if (cache != nullptr && cache->mReady && cache->mW == W && cache->mIntFree == intFree) {
            m_chol = &cache->cholM;
            m_D = &cache->mD;
        } else {
            if (!factorAndInvertIntBlock(backend, nCut, rows, tuvRows, ttRows, intFree, cache,
                                         cancel)) {
                return false;
            }
        }
        // x0 = (M+ridge)^-1 g — the exact relaxed reduced solution.
        std::vector<double> g(W);
        for (std::size_t i = 0; i < W; ++i) {
            g[i] = static_cast<double>(gReduced[i]);
        }
        m_chol->solve(g, m_x0);
        m_inS.assign(nInt, 0);
        m_S.clear();
        m_c.clear();
        m_dchol = DenseCholeskyInc(nInt);
        return true;
    }

    // The exact relaxed solution, as the float w the greedy loop reads.
    void seed(std::vector<float>& w) const {
        for (std::size_t j = 0; j < m_W; ++j) {
            w[j] = static_cast<float>(m_x0[j]);
        }
    }

    // One rounding round: absorb the newly pinned integers (mask flipped to 0;
    // w already carries their clamped values) into the bordered factor, then
    // refresh w on every still-free integer coordinate. Returns false when the
    // dense factor loses positivity (callers fall back to the masked CG).
    bool resolve(const std::vector<char>& mask, std::vector<float>& w) {
        const std::vector<std::size_t>& intFree = *m_intFree;
        const std::size_t nInt = intFree.size();
        std::vector<double> col;
        for (std::size_t k = 0; k < nInt; ++k) {
            if (m_inS[k] || mask[intFree[k]] != 0) {
                continue;
            }
            col.resize(m_S.size());
            for (std::size_t i = 0; i < m_S.size(); ++i) {
                col[i] = (*m_D)[k * nInt + m_S[i]];
            }
            if (!m_dchol.add(col, (*m_D)[k * nInt + k])) {
                return false;
            }
            m_S.push_back(k);
            m_c.push_back(static_cast<double>(w[intFree[k]]));
            m_inS[k] = 1;
        }
        std::vector<double> rhs(m_S.size());
        for (std::size_t i = 0; i < m_S.size(); ++i) {
            rhs[i] = m_x0[intFree[m_S[i]]] - m_c[i];
        }
        m_dchol.solve(rhs, m_lambda);
        for (std::size_t k = 0; k < nInt; ++k) {
            if (m_inS[k]) {
                continue;
            }
            double t = m_x0[intFree[k]];
            const double* dRow = m_D->data() + k * nInt;
            for (std::size_t i = 0; i < m_S.size(); ++i) {
                t -= dRow[m_S[i]] * m_lambda[i];
            }
            w[intFree[k]] = static_cast<float>(t);
        }
        return true;
    }

    // Exact full solution for the current pin set: w = x0 - (M+ridge)^-1 E^T
    // lambda on every free coordinate. Pinned coordinates (mask == 0) keep the
    // exact integers the schedule already wrote into w.
    void finalize(const std::vector<char>& mask, std::vector<float>& w) const {
        std::vector<double> full(m_x0);
        if (!m_S.empty()) {
            const std::vector<std::size_t>& intFree = *m_intFree;
            std::vector<double> rhs(m_W, 0.0);
            std::vector<double> corr;
            for (std::size_t i = 0; i < m_S.size(); ++i) {
                rhs[intFree[m_S[i]]] = m_lambda[i];
            }
            m_chol->solve(rhs, corr);
            for (std::size_t j = 0; j < m_W; ++j) {
                full[j] -= corr[j];
            }
        }
        for (std::size_t j = 0; j < m_W; ++j) {
            if (mask[j] != 0) {
                w[j] = static_cast<float>(full[j]);
            }
        }
    }

private:
    // Build + factor M and precompute D, storing into the cache when one is
    // provided (reused across calibration attempts) or locally otherwise.
    bool factorAndInvertIntBlock(
        accel::IBackend& backend, std::size_t nCut,
        const std::vector<std::unordered_map<std::size_t, float>>& rows,
        const std::vector<std::vector<std::pair<std::size_t, double>>>& tuvRows,
        const std::vector<std::vector<std::pair<std::size_t, double>>>& ttRows,
        const std::vector<std::size_t>& intFree, SeamlessSolveCacheImpl* cache,
        const CancelToken* cancel) {
        const std::size_t nInt = intFree.size();
        SparseCholesky* chol = cache != nullptr ? &cache->cholM : &m_ownChol;
        std::vector<double>* D = cache != nullptr ? &cache->mD : &m_ownD;
        if (cache != nullptr) {
            cache->mReady = false;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::size_t> mStart, mCol;
        std::vector<double> mVal;
        buildReducedOperator(nCut, rows, tuvRows, ttRows, m_W, mStart, mCol, mVal);
        const long msM = msSince(t0);
        if (cancel != nullptr && cancel->isCancelled()) {
            return false;
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (!chol->factor(m_W, mStart, mCol, mVal, kReducedRidge)) {
            return false;
        }
        const long msFactor = msSince(t1);
        if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
            std::size_t nnzL = 0;
            for (const auto& r : rows) {
                nnzL += r.size();
            }
            std::fprintf(stderr,
                         "[qc] direct reduced: W=%zu nnzM=%zu cholNnz=%zu (L2 nnz=%zu) "
                         "intFree=%zu\n",
                         m_W, mCol.size(), chol->factorNnz(), 2 * nnzL, nInt);
        }
        if (cancel != nullptr && cancel->isCancelled()) {
            return false;
        }
        const auto t2 = std::chrono::steady_clock::now();
        D->assign(nInt * nInt, 0.0);
        double* dOut = D->data();
        const SparseCholesky* ch = chol;
        backend.parallelFor(0, nInt, [ch, &intFree, dOut, nInt](std::size_t lo, std::size_t hi) {
            for (std::size_t k = lo; k < hi; ++k) {
                ch->solveUnitGather(intFree[k], intFree, dOut + k * nInt);
            }
        });
        if (std::getenv("CYBER_QC_TIME") != nullptr) {
            std::fprintf(stderr, "[qc-time] direct reduced: M=%ldms factor=%ldms D=%ldms\n", msM,
                         msFactor, msSince(t2));
        }
        if (cache != nullptr) {
            cache->mW = m_W;
            cache->mIntFree = intFree;
            cache->mReady = true;
        }
        m_chol = chol;
        m_D = D;
        return true;
    }

    std::size_t m_W = 0;
    const std::vector<std::size_t>* m_intFree = nullptr;
    const SparseCholesky* m_chol = nullptr;
    const std::vector<double>* m_D = nullptr;
    SparseCholesky m_ownChol;      // storage when no cache is provided
    std::vector<double> m_ownD;    // "
    std::vector<double> m_x0;      // exact relaxed reduced solution
    std::vector<std::size_t> m_S;  // pinned integers, in pin order
    std::vector<double> m_c;       // their clamped integer values
    std::vector<double> m_lambda;  // bordered-system multipliers
    std::vector<char> m_inS;
    DenseCholeskyInc m_dchol{0};
};

// Coefficients of R^rho applied to a (u,v) pair, R = CCW (u,v) -> (-v,u):
//   (R^rho (u,v))_x = cxu*u + cxv*v ;  (R^rho (u,v))_y = cyu*u + cyv*v.
void rotCoeffs(int rho, double& cxu, double& cxv, double& cyu, double& cyv) {
    switch (((rho % 4) + 4) % 4) {
        case 0:
            cxu = 1;
            cxv = 0;
            cyu = 0;
            cyv = 1;
            break;
        case 1:
            cxu = 0;
            cxv = -1;
            cyu = 1;
            cyv = 0;
            break;
        case 2:
            cxu = -1;
            cxv = 0;
            cyu = 0;
            cyv = -1;
            break;
        default:
            cxu = 0;
            cxv = 1;
            cyu = -1;
            cyv = 0;
            break;
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

// Combed representative cross direction of face f. The comb (and the period jumps it is
// built from) lives on the angle() convention (theta in [0, pi/2)), while direction()
// returns the raw atan2 branch (theta in (-pi/4, pi/4]) — a per-face quarter-turn offset
// wherever raw theta < 0. Reconstructing e0 from direction() under an angle()-based comb
// therefore yields a QUARTER-TURN-MIXED target field on a single coplanar patch (box top:
// 178 x-like vs 110 y-like faces measured) whose Poisson compromise is the ~32-degree
// uniform shear of the feature-pinning lever. Under the lever the reconstruction uses the
// SAME angle() convention the comb was built on; the historical direction() branch is kept
// for the lever-off path (bit-compatible).
Vec3 combedDirection(const CrossField& field, FaceId f, const Vec3& n, int comb,
                     bool angleConvention) {
    if (!angleConvention) {
        return rotQuarter(field.direction(f), n, comb);
    }
    const float th = field.angle(f);
    const Vec3 d = field.tangent[f.value] * std::cos(th) + field.bitangent[f.value] * std::sin(th);
    return rotQuarter(d, n, comb);
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
    // Feature (crease) seam: the coordinate `pinAxis` (0=u, 1=v) is constant
    // along the edge on side A and pinned to the integer lattice, so an
    // integer isoline runs exactly along the crease and extraction traces it
    // (its edge-on-isoline case). This is the piece whose absence made
    // feature tagging alone REGRESS: hard seams without integer pinning give
    // every feature-bounded patch an independent grid phase, so the patch
    // grids disagree along the crease and extraction leaves cracks.
    bool feature = false;
    int pinAxis = 0;
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
                         const CancelToken* cancel = nullptr,
                         SeamlessSolveCacheImpl* cache = nullptr,
                         bimdf::Charts* bimdfCharts = nullptr) {
    const std::size_t nSeam = seams.size();
    const std::size_t nUv = 2 * nCut;
    // Feature-seam integer pinning (docs/ROADMAP.md 2026-08-01 priority 1;
    // successor to the refuted M2d — the two conditions that made M2d inert
    // are now satisfied for the meshes this reaches: crease-heavy inputs
    // route native, and the cross field is hard-pinned along tagged feature
    // edges so a constant coordinate along the crease exists to pin). Each
    // feature seam edge gets one extra promoted INTEGER variable c_e and two
    // homogeneous rows: coord(aA) - coord(bA) = 0 (the crease is a level set
    // of that coordinate) and coord(aA) - c_e = 0 (the level set lands on the
    // integer lattice, i.e. ON an isoline). Kill switch: CYBER_QC_NO_FEATURE_PIN.
    const bool pinFeatures = std::getenv("CYBER_QC_NO_FEATURE_PIN") == nullptr;
    std::vector<std::size_t> featureSeams;
    if (pinFeatures) {
        for (std::size_t e = 0; e < nSeam; ++e) {
            if (seams[e].feature) {
                featureSeams.push_back(e);
            }
        }
    }
    const std::size_t nFeat = featureSeams.size();
    const std::size_t N = nUv + 2 * nSeam + nFeat;
    const auto uIx = [](std::size_t c) { return c; };
    const auto vIx = [nCut](std::size_t c) { return nCut + c; };
    const auto txIx = [nCut](std::size_t e) { return 2 * nCut + 2 * e; };
    const auto tyIx = [nCut](std::size_t e) { return 2 * nCut + 2 * e + 1; };
    const auto cIx = [nCut, nSeam](std::size_t k) { return 2 * nCut + 2 * nSeam + k; };

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
        const std::array<std::pair<std::size_t, std::size_t>, 2> ends{std::make_pair(s.aA, s.aB),
                                                                      std::make_pair(s.bA, s.bB)};
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
    // Feature-seam rows: crease level set + integer pinning (see above).
    for (std::size_t k = 0; k < nFeat; ++k) {
        const SeamRef& s = seams[featureSeams[k]];
        const auto coord = [&](std::size_t c) { return s.pinAxis == 0 ? uIx(c) : vIx(c); };
        Row level;
        addC(level, coord(s.aA), 1.0);
        addC(level, coord(s.bA), -1.0);
        prune(level);
        if (!level.empty()) {
            cons.push_back(std::move(level));
        }
        Row lattice;
        addC(lattice, coord(s.aA), 1.0);
        addC(lattice, cIx(k), -1.0);
        cons.push_back(std::move(lattice));
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
    // j as an affine-free combination of the surviving independent variables.
    //
    // Pivot scoring decides whether the rounded map is actually an INTEGER grid: after the
    // greedy rounding pins the free integers, every DEPENDENT integer (seam translation /
    // crease lattice offset) must still evaluate to an integer, which requires its pivot
    // expression to (a) touch no continuous free and (b) carry integer coefficients.
    // Neither static preference achieves that on every mesh:
    //   - the historical continuous-first scoring picks NON-UNIT continuous pivots, whose
    //     fractional expressions spread into integer-only rows (box_sharp: 96/144 crease
    //     seams landed at offsets up to 3/8 of a cell — the patch grids disagreed along
    //     every crease, extraction traced the cube caps as disconnected islands, and whole
    //     faces were silently dropped);
    //   - a unit-first scoring keeps coefficients integer-exact but pivots integers out of
    //     rows that still hold continuous frees, leaving translations tied to unrounded
    //     continuous DOF (the cylinder's closed rim loops: 39/96 fractional rim seams
    //     vs 10 historical, singularities 11 -> 52 at 900).
    // So run the elimination BOTH ways and keep the reduction with fewer structural
    // integrality violations; ties keep the historical one (byte-stable on meshes both
    // handle, incl. every feature-free organic). CYBER_QC_NO_UNIT_PIVOT pins the
    // historical scoring, CYBER_QC_UNIT_PIVOT the unit-first one (A/B).
    struct Reduction {
        std::vector<char> isPivot;
        std::vector<Row> pivotExpr;
    };
    const auto eliminate = [&](bool unitFirst) {
        Reduction red;
        red.isPivot.assign(N, 0);
        red.pivotExpr.assign(N, Row{});
        std::vector<char>& isPivot = red.isPivot;
        std::vector<Row>& pivotExpr = red.pivotExpr;
        for (const Row& raw : cons) {
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
                const int score =
                    unitFirst ? (unit ? 2 : 0) + (cont ? 1 : 0) : (cont ? 2 : 0) + (unit ? 1 : 0);
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
            // Back-substitute the new pivot into every existing pivot expression
            // (Gauss-Jordan), so all pivot expressions stay in independent variables only.
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
        return red;
    };
    // Structural integrality violations: dependent integers whose expression reaches a
    // continuous free or carries a non-integer coefficient on an integer free.
    const auto integralityViolations = [&](const Reduction& red) {
        std::size_t bad = 0;
        for (std::size_t j = nUv; j < N; ++j) {
            if (!red.isPivot[j]) {
                continue;
            }
            for (const auto& [c, w] : red.pivotExpr[j]) {
                if (c < nUv || std::abs(w - std::round(w)) > 1e-6) {
                    ++bad;
                    break;
                }
            }
        }
        return bad;
    };
    Reduction red;
    if (std::getenv("CYBER_QC_NO_UNIT_PIVOT") != nullptr) {
        red = eliminate(false);
    } else if (std::getenv("CYBER_QC_UNIT_PIVOT") != nullptr) {
        red = eliminate(true);
    } else {
        Reduction hist = eliminate(false);
        const std::size_t histBad = integralityViolations(hist);
        if (histBad == 0) {
            red = std::move(hist);  // historical reduction already integer-exact
        } else {
            Reduction unit = eliminate(true);
            const std::size_t unitBad = integralityViolations(unit);
            red = unitBad < histBad ? std::move(unit) : std::move(hist);
            if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
                std::fprintf(stderr, "[qc] pivot integrality: hist=%zu unit=%zu -> %s\n", histBad,
                             unitBad, unitBad < histBad ? "unit" : "hist");
            }
        }
    }
    const std::vector<char>& isPivot = red.isPivot;
    const std::vector<Row>& pivotExpr = red.pivotExpr;

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
    const auto buildCsr = [](std::size_t nr,
                             const std::vector<std::vector<std::pair<std::size_t, double>>>& r) {
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
    // Effective gradient of the masked solves: gReduced, plus the linear term
    // of the guided-rounding attraction while it is active (bit-identical
    // copy otherwise).
    std::vector<float> gSolve = gReduced;

    // Direct sparse-Cholesky path (CYBER_QC_DIRECT, default on; docs/ROADMAP.md
    // perf entry): the reduced operator is FIXED across the relaxed solve and
    // every greedy rounding round, so one factorization + bordered (Woodbury)
    // pin updates replace the ~50 masked CG solves. CYBER_QC_NO_DIRECT reverts
    // to the historical masked CG byte-exactly; CYBER_QC_FUSED_OPERATOR keeps
    // the CG loop but applies the operator as ONE explicit spmv instead of
    // three chained ones (the fill-pathology fallback; not byte-identical —
    // float sums re-associate).
    const bool noDirect = std::getenv("CYBER_QC_NO_DIRECT") != nullptr;
    const bool fusedForced = !noDirect && std::getenv("CYBER_QC_FUSED_OPERATOR") != nullptr;
    accel::SparseMatrix fusedM;
    bool haveFused = false;
    if (fusedForced) {
        std::vector<std::size_t> mStart, mCol;
        std::vector<double> mVal;
        buildReducedOperator(nCut, rows, tuvRows, ttRows, W, mStart, mCol, mVal);
        fusedM.rows = W;
        fusedM.rowStart = std::move(mStart);
        fusedM.colIndex = std::move(mCol);
        fusedM.value.assign(mVal.begin(), mVal.end());
        haveFused = true;
    }

    // Guided-rounding attraction (CYBER_QC_BIMDF=guided, filled after the
    // Bi-MDF solve): the rounding schedule's re-solves minimize
    //   E(w) + 1/2 sum_r mu_r (row_r . w - len_r)^2
    // over the T-mesh arc rows, pulling the RELAXED values toward the flow's
    // arc assignment so the untouched greedy schedule rounds toward the
    // flow's structure — every intermediate state stays integral and
    // seamless by greedy's own invariant, and coupled integers drift
    // together instead of being steered one coordinate at a time (measured
    // to lose: coordinate-wise floor/ceil steering cost nefertiti@4000 sing
    // 396 -> 418..422 across three scoring variants). Empty rows (the flag's
    // default state) keep the operator byte-identical; the final solve after
    // the last pin always runs unattracted so the continuous DOF are pure
    // Dirichlet over the chosen integers.
    struct SteerRow {
        std::vector<std::pair<std::size_t, double>> a;  // (reduced index, coeff)
        double len = 0.0;                               // flow arc length, cells
        double mu = 0.0;                                // attraction weight
    };
    std::vector<SteerRow> steerRows;
    bool steering = false;

    // Reduced operator A(p) = Tt * L2 * Tuv * p, applied via three spmv (or the
    // one fused spmv when CYBER_QC_FUSED_OPERATOR built it explicitly), plus
    // the guided-rounding attraction rows when active.
    accel::Buffer<float> tmpUv, tmpUv2, outBuf, pBuf;
    const auto applyA = [&](const std::vector<float>& p, std::vector<float>& out) {
        pBuf.upload(p);
        if (haveFused) {
            accel::spmv(backend, fusedM, pBuf, outBuf);
        } else {
            accel::spmv(backend, Tuv, pBuf, tmpUv);
            accel::spmv(backend, L2, tmpUv, tmpUv2);
            accel::spmv(backend, Tt, tmpUv2, outBuf);
        }
        out.assign(outBuf.data(), outBuf.data() + outBuf.size());
        if (steering) {
            for (const SteerRow& sr : steerRows) {
                double s = 0.0;
                for (const auto& [ri, cf] : sr.a) {
                    s += cf * static_cast<double>(p[ri]);
                }
                s *= sr.mu;
                for (const auto& [ri, cf] : sr.a) {
                    out[ri] += static_cast<float>(cf * s);
                }
            }
        }
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
            rhs[i] = mask[i] ? (gSolve[i] - Aw[i]) : 0.0f;
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
    // The direct engine, when it initializes, serves the exact same solutions from one
    // factorization; the schedule below is shared verbatim by both solvers.
    std::vector<char> mask(W, 1);
    DirectRounding direct;
    bool useDirect =
        !noDirect && !haveFused &&
        direct.init(backend, nCut, rows, tuvRows, ttRows, gReduced, intFree, W, cache, cancel);
    if (useDirect) {
        ++maskedSolveCalls;
        direct.seed(w);
    } else {
        maskedSolve(mask);
    }

    // Quantization scoreboard (CYBER_QC_DEBUG): the reduced Dirichlet energy
    // E(w) = 0.5 w^T (M + ridge) w - gReduced^T w, the exact objective the
    // integer assignment shapes. Reported at the relaxed optimum and after the
    // final constrained solve, so alternative quantizers (greedy vs Bi-MDF)
    // are A/B-comparable on identical reductions (same M, gReduced, x0).
    const bool dbg = std::getenv("CYBER_QC_DEBUG") != nullptr;
    const auto reducedEnergy = [&]() {
        std::vector<float> Aw;
        applyA(w, Aw);
        double e = 0.0;
        for (std::size_t i = 0; i < W; ++i) {
            const double wi = static_cast<double>(w[i]);
            e += wi * (0.5 * (static_cast<double>(Aw[i]) + ridge * wi) -
                       static_cast<double>(gReduced[i]));
        }
        return e;
    };
    const double eRelaxed = dbg ? reducedEnergy() : 0.0;

    // MAGNITUDE CONTROL. The reduction leaves the independent integer translations
    // UNCONSTRAINED — any integers keep the map seamless — so a huge relaxed value would
    // round to a huge integer and blow the map up (the failure the divergence guard used to
    // reject). Bound each rounded translation to a physically sane range: a seamless integer
    // grid crosses at most O(grid extent) cells along any seam, so cap by the number of grid
    // lines the relaxed UV already spans (plus slack). Clamping here keeps seamlessness intact
    // and simply refuses to place a seam translation the geometry never justified.
    double uvSpan = 0.0;
    std::vector<float> relaxedUv;  // z = Tuv w at the relaxed optimum (Bi-MDF substrate)
    {
        accel::Buffer<float> wProbe(w), zProbe;
        accel::spmv(backend, Tuv, wProbe, zProbe);
        for (std::size_t i = 0; i < zProbe.size(); ++i) {
            uvSpan = std::max(uvSpan, static_cast<double>(std::abs(zProbe[i])));
        }
        if (bimdfCharts != nullptr) {
            relaxedUv.assign(zProbe.data(), zProbe.data() + zProbe.size());
        }
    }
    const double tCap = std::max(uvSpan * 2.0, std::sqrt(static_cast<double>(nCut))) + 8.0;
    const auto clampInt = [tCap](double val) { return std::clamp(std::round(val), -tCap, tCap); };

    // Bi-MDF quantization (CYBER_QC_BIMDF, openspec/changes/bimdf-quantization):
    // build the motorcycle-graph T-mesh on the relaxed seamless map, assign
    // integer arc lengths by min-deviation flow, and map the assignment back
    // onto the free-integer basis. CYBER_QC_BIMDF=report solves and reports
    // without injecting (A/B mode). Any failure at any stage falls back to
    // the untouched greedy schedule below.
    std::unique_ptr<bimdf::TMesh> bimdfTm;
    std::vector<std::vector<std::pair<std::size_t, double>>> bimdfArcRows;
    std::vector<std::pair<std::size_t, double>> bimdfPins;  // (intFree ordinal, value)
    if (bimdfCharts != nullptr) {
        bimdfCharts->u = relaxedUv.data();
        bimdfCharts->v = relaxedUv.data() + nCut;
        // Relaxed values of every promoted variable (diagnostics: the symbolic
        // arc lengths must reproduce the traced numeric lengths).
        std::vector<double> zRel(N, 0.0);
        for (std::size_t i = 0; i < nUv; ++i) {
            zRel[i] = static_cast<double>(relaxedUv[i]);
        }
        for (std::size_t e = 0; e < nSeam; ++e) {
            const SeamRef& s = seams[e];
            double cxu, cxv, cyu, cyv;
            rotCoeffs(s.rho, cxu, cxv, cyu, cyv);
            const double uA = zRel[uIx(s.aA)], vA = zRel[vIx(s.aA)];
            zRel[txIx(e)] = zRel[uIx(s.aB)] - (cxu * uA + cxv * vA);
            zRel[tyIx(e)] = zRel[vIx(s.aB)] - (cyu * uA + cyv * vA);
        }
        for (std::size_t k = 0; k < nFeat; ++k) {
            const SeamRef& s = seams[featureSeams[k]];
            zRel[cIx(k)] = s.pinAxis == 0 ? zRel[uIx(s.aA)] : zRel[vIx(s.aA)];
        }
        bimdfTm = std::make_unique<bimdf::TMesh>(bimdf::buildTMesh(*bimdfCharts, zRel));
        const bimdf::TMesh& tmesh = *bimdfTm;
        std::fprintf(
            stderr,
            "[qc] bimdf tmesh: ok=%d nodes=%zu arcs=%zu patches=%zu cones=%zu "
            "tnodes=%zu rays=%zu steps=%zu failedRays=%zu degraded=%zu repaired=%zu "
            "twinMerges=%zu spurs=%zu excluded=%zu exprErr=%.2e sideMismatch=%.3f%s%s%s%s\n",
            tmesh.ok ? 1 : 0, tmesh.nodeCount, tmesh.arcs.size(), tmesh.patches.size(),
            tmesh.coneNodes, tmesh.tNodes, tmesh.raysTraced, tmesh.raySteps, tmesh.failedRays,
            tmesh.degradedNodes, tmesh.repairedNodes, tmesh.twinMerges, tmesh.spurCollapses,
            tmesh.excludedPatches, tmesh.maxExprErr, tmesh.maxSideMismatch,
            tmesh.reason.empty() ? "" : " reason=", tmesh.reason.c_str(),
            tmesh.rejectSummary.empty() ? "" : " ", tmesh.rejectSummary.c_str());
        // Reduce every arc-length expression onto the reduced basis w (used
        // for the back-substitution and the post-solve deviation report).
        if (tmesh.ok) {
            std::vector<std::size_t> ordinalOf(W, kInvalidIndex);
            for (std::size_t k = 0; k < intFree.size(); ++k) {
                ordinalOf[intFree[k]] = k;
            }
            bimdfArcRows.resize(tmesh.arcs.size());
            for (std::size_t a = 0; a < tmesh.arcs.size(); ++a) {
                std::unordered_map<std::size_t, double> row;
                for (const auto& [zv, cf] : tmesh.arcs[a].expr) {
                    if (isPivot[zv]) {
                        for (const auto& [c2, w2] : pivotExpr[zv]) {
                            row[freeIx[c2]] += cf * w2;
                        }
                    } else {
                        row[freeIx[zv]] += cf;
                    }
                }
                auto& out = bimdfArcRows[a];
                for (const auto& [ri, cf] : row) {
                    if (std::abs(cf) > 1e-7) {
                        out.push_back({ri, cf});
                    }
                }
                std::sort(out.begin(), out.end());
            }
            const bimdf::BimdfResult sol = bimdf::solveBimdf(tmesh);
            std::fprintf(
                stderr,
                "[qc] bimdf solve: ok=%d energy=%.3f raisedToMin=%zu parityFlips=%zu "
                "halfIntegral=%zu sideViolation=%lld poly=%zu polyOdd=%zu dropped=%zu%s%s\n",
                sol.ok ? 1 : 0, sol.deviationEnergy, sol.raisedToMin, sol.parityFlips,
                sol.halfIntegral, sol.maxSideViolation, sol.polyPatches, sol.polyOddSum,
                sol.droppedPatches, sol.reason.empty() ? "" : " reason=", sol.reason.c_str());
            if (sol.ok) {
                // Health gates for the guided attraction — it is an
                // ORGANIC-mesh lever:
                // (1) Side consistency: the steering targets are only as
                //     good as the T-mesh's relaxed side sums. Measured at
                //     mu=1: spot (mismatch 0.001) and stanford-bunny (0.065)
                //     improve hard (bunny sing 135 -> 94, ears 37 -> 19;
                //     spot pure flow-loop 763 -> 1274), while the
                //     fold-damaged nefertiti (0.435) and armadillo (0.496)
                //     REGRESS under the same steering (sing 396 -> 422 /
                //     255 -> 285) — their flow assignments inherit the
                //     damage.
                // (2) Crease-dominated T-meshes: on creased CAD the
                //     attraction fights the pinned crease lattice — the CAD
                //     sweep measured cylinder@4500 pure collapsing (quads
                //     3792 -> 1702, haus 0.0048 -> 0.0499) and
                //     cylinder@900/@1400 mixed sing 8 -> 18 / 2 -> 7 with
                //     steering on; exact injection (unchanged semantics)
                //     still serves CAD. Threshold 1% of arcs: cylinder/box/
                //     fandisk sit at 7.6-18% crease arcs, the organics at 0
                //     (stanford-bunny: 1 of 1338).
                // Refused T-meshes keep the pure greedy schedule. Round 5
                // measured the alternative (per-arc health so damaged meshes
                // engage on their clean regions): sing regressed on every
                // fold-damaged cell it opened (nefertiti mixed 396 -> 400,
                // armadillo mixed 255 -> 281, armadillo pure 273 -> 293,
                // nefertiti pure 419 -> 411 flat; mu=2 forced it to 310 but
                // cost haus 0.0072 -> 0.0194 and 14% of the quads) — the
                // whole-mesh gate stays, with CYBER_QC_BIMDF_HEALTH as the
                // measurement override. Inside an ENGAGED mesh the per-arc
                // filter below still drops arcs of inconsistent quads
                // (no-op on the healthy meshes: bunny unhealthyQuads=0).
                const char* healthEnv = std::getenv("CYBER_QC_BIMDF_HEALTH");
                const double kSteerHealthMax = healthEnv != nullptr ? std::atof(healthEnv) : 0.2;
                std::size_t featureArcs = 0;
                for (const bimdf::Arc& arc : tmesh.arcs) {
                    featureArcs += arc.onFeature ? 1 : 0;
                }
                std::vector<char> arcSteerable(tmesh.arcs.size(), 1);
                std::size_t unhealthyQuads = 0;
                for (const bimdf::Patch& p : tmesh.patches) {
                    if (!p.isQuad()) {
                        continue;
                    }
                    double mm = 0.0;
                    for (int k = 0; k < 2; ++k) {
                        double s0 = 0.0, s1 = 0.0;
                        for (const std::size_t a : p.side[static_cast<std::size_t>(k)]) {
                            s0 += tmesh.arcs[a].len;
                        }
                        for (const std::size_t a : p.side[static_cast<std::size_t>(k + 2)]) {
                            s1 += tmesh.arcs[a].len;
                        }
                        mm = std::max(mm, std::abs(s0 - s1));
                    }
                    if (mm <= kSteerHealthMax) {
                        continue;
                    }
                    ++unhealthyQuads;
                    for (const auto& sideArcs : p.side) {
                        for (const std::size_t a : sideArcs) {
                            arcSteerable[a] = 0;
                        }
                    }
                }
                const char* modeEnv = std::getenv("CYBER_QC_BIMDF");
                if (modeEnv != nullptr && std::string(modeEnv) == "guided" &&
                    (tmesh.maxSideMismatch > kSteerHealthMax ||
                     featureArcs * 100 > tmesh.arcs.size())) {
                    std::fprintf(stderr,
                                 "[qc] bimdf guided: refused (sideMismatch %.3f, "
                                 "featureArcs %zu), pure greedy\n",
                                 tmesh.maxSideMismatch, featureArcs);
                    modeEnv = nullptr;
                }
                if (modeEnv != nullptr && std::string(modeEnv) == "guided") {
                    // Steering targets come from a floors-OFF solve: the side
                    // floors inflate the shipped assignment past the greedy
                    // realization (nefertiti@4000 energy 2711 vs greedy 1879
                    // — nothing worth steering toward), while the floor-free
                    // flow decides open-vs-closed per strip honestly
                    // (energy 1289 on the same T-mesh).
                    bimdf::BimdfOptions steerOpts;
                    steerOpts.sideFloors = false;
                    const bimdf::BimdfResult solS = bimdf::solveBimdf(tmesh, steerOpts);
                    const bimdf::BimdfResult& src = solS.ok ? solS : sol;
                    const char* muEnv = std::getenv("CYBER_QC_BIMDF_MU");
                    const double muBase = muEnv != nullptr ? std::atof(muEnv) : 1.0;
                    for (std::size_t a = 0; a < tmesh.arcs.size(); ++a) {
                        // Contained regions have no flow assignment to steer
                        // toward; their integers round pure-greedy. Feature
                        // arcs ARE steered — excluding them was measured to
                        // cost the fandisk mixed win (sing 43 -> 49) without
                        // fixing the fandisk pure regression (117 -> 119).
                        if ((a < tmesh.arcExcluded.size() && tmesh.arcExcluded[a] != 0) ||
                            (a < src.arcOutside.size() && src.arcOutside[a] != 0) ||
                            arcSteerable[a] == 0 || bimdfArcRows[a].empty()) {
                            continue;
                        }
                        SteerRow sr;
                        sr.a = bimdfArcRows[a];
                        sr.len = 0.5 * static_cast<double>(src.arcLenHalf[a]);
                        // Mirror the Bi-MDF deviation normalization: long
                        // arcs tolerate more absolute deviation.
                        sr.mu = muBase / std::max(tmesh.arcs[a].len, 0.5);
                        steerRows.push_back(std::move(sr));
                    }
                    std::fprintf(stderr,
                                 "[qc] bimdf guided: steerSolve ok=%d energy=%.3f mu=%.3f "
                                 "steerRows=%zu of %zu (unhealthyQuads=%zu mm=%.3f)\n",
                                 solS.ok ? 1 : 0, solS.deviationEnergy, muBase, steerRows.size(),
                                 tmesh.arcs.size(), unhealthyQuads, tmesh.maxSideMismatch);
                }
                // Back-substitution A y = len over the free integers. Rows
                // touching a continuous free are the reduction's structural
                // integrality violations and cannot be enforced.
                struct SysRow {
                    std::vector<std::pair<std::size_t, double>> a;  // (ordinal, coeff)
                    double rhs = 0.0;
                    std::size_t arc = 0;  // source arc (for containment drops)
                };
                // Injection-layer containment: an inconsistent equation
                // (fractional pivot residue) marks its arc as excluded and
                // the elimination reruns without it — the greedy rounding
                // absorbs the dropped region, the consistent remainder is
                // still injected.
                std::vector<char> injDrop(tmesh.arcs.size(), 0);
                std::size_t injDropped = 0;
                if (std::getenv("CYBER_QC_BIMDF_DEBUG") != nullptr) {
                    // Lattice-parity census: a row whose reduced coefficients
                    // are all integers can only realize EVEN half-cell arc
                    // lengths over integer y; an odd assignment there is
                    // structurally uninjectable.
                    std::size_t intRows = 0, halfRows = 0, oddOnInt = 0, otherRows = 0;
                    for (std::size_t a = 0; a < tmesh.arcs.size(); ++a) {
                        if (bimdfArcRows[a].empty()) {
                            continue;
                        }
                        bool allInt = true, allHalf = true;
                        for (const auto& [ri, cf] : bimdfArcRows[a]) {
                            if (std::abs(cf - std::round(cf)) > 1e-6) {
                                allInt = false;
                            }
                            if (std::abs(2.0 * cf - std::round(2.0 * cf)) > 1e-6) {
                                allHalf = false;
                            }
                        }
                        if (allInt) {
                            ++intRows;
                            if (sol.arcLenHalf[a] % 2 != 0) {
                                ++oddOnInt;
                            }
                        } else if (allHalf) {
                            ++halfRows;
                        } else {
                            ++otherRows;
                        }
                    }
                    std::fprintf(stderr,
                                 "[qc] bimdf parity census: intRows=%zu (oddAssigned=%zu) "
                                 "halfRows=%zu otherRows=%zu\n",
                                 intRows, oddOnInt, halfRows, otherRows);
                }
                std::size_t badArcs = 0;
                std::size_t exclArcs = 0;
                constexpr int kInjectRounds = 6;
                for (int round = 0; round < kInjectRounds; ++round) {
                    std::vector<SysRow> sys;
                    badArcs = 0;
                    exclArcs = 0;
                    for (std::size_t a = 0; a < tmesh.arcs.size(); ++a) {
                        // Locally contained regions (rejected orbits,
                        // abandoned launches, patches dropped by the solve's
                        // infeasibility valve, inconsistent rows from earlier
                        // rounds): their arcs' integers stay with the greedy
                        // rounding below.
                        if ((a < tmesh.arcExcluded.size() && tmesh.arcExcluded[a] != 0) ||
                            (a < sol.arcOutside.size() && sol.arcOutside[a] != 0) ||
                            injDrop[a] != 0) {
                            ++exclArcs;
                            continue;
                        }
                        SysRow row;
                        row.arc = a;
                        bool good = true;
                        for (const auto& [ri, cf] : bimdfArcRows[a]) {
                            const std::size_t ord = ordinalOf[ri];
                            if (ord == kInvalidIndex) {
                                good = false;
                                break;
                            }
                            row.a.push_back({ord, cf});
                        }
                        if (!good || row.a.empty()) {
                            ++badArcs;
                            continue;
                        }
                        row.rhs = 0.5 * static_cast<double>(sol.arcLenHalf[a]);
                        sys.push_back(std::move(row));
                    }
                    std::vector<std::size_t> cols;
                    {
                        std::vector<char> seen(intFree.size(), 0);
                        for (const SysRow& row : sys) {
                            for (const auto& [ord, cf] : row.a) {
                                if (!seen[ord]) {
                                    seen[ord] = 1;
                                    cols.push_back(ord);
                                }
                            }
                        }
                        std::sort(cols.begin(), cols.end());
                    }
                    std::vector<std::size_t> colOf(intFree.size(), kInvalidIndex);
                    for (std::size_t c = 0; c < cols.size(); ++c) {
                        colOf[cols[c]] = c;
                    }
                    const std::size_t nR = sys.size(), nC = cols.size();
                    std::vector<double> A2(nR * nC, 0.0), rhs2(nR, 0.0);
                    for (std::size_t rI = 0; rI < nR; ++rI) {
                        for (const auto& [ord, cf] : sys[rI].a) {
                            A2[rI * nC + colOf[ord]] += cf;
                        }
                        rhs2[rI] = sys[rI].rhs;
                    }
                    // Gauss-Jordan with partial pivoting; non-pivot unknowns
                    // take their rounded relaxed values (the greedy default),
                    // pivots follow the flow assignment.
                    std::vector<std::size_t> pivotColOfRow(nR, kInvalidIndex);
                    std::vector<char> colUsed(nC, 0);
                    for (std::size_t rI = 0; rI < nR; ++rI) {
                        std::size_t pc = kInvalidIndex;
                        double best = 1e-7;
                        for (std::size_t c = 0; c < nC; ++c) {
                            if (!colUsed[c] && std::abs(A2[rI * nC + c]) > best) {
                                best = std::abs(A2[rI * nC + c]);
                                pc = c;
                            }
                        }
                        if (pc == kInvalidIndex) {
                            continue;  // dependent row
                        }
                        colUsed[pc] = 1;
                        pivotColOfRow[rI] = pc;
                        const double inv = 1.0 / A2[rI * nC + pc];
                        for (std::size_t c = 0; c < nC; ++c) {
                            A2[rI * nC + c] *= inv;
                        }
                        rhs2[rI] *= inv;
                        for (std::size_t r2 = 0; r2 < nR; ++r2) {
                            if (r2 == rI || A2[r2 * nC + pc] == 0.0) {
                                continue;
                            }
                            const double f = A2[r2 * nC + pc];
                            for (std::size_t c = 0; c < nC; ++c) {
                                A2[r2 * nC + c] -= f * A2[rI * nC + c];
                            }
                            rhs2[r2] -= f * rhs2[rI];
                        }
                    }
                    std::vector<double> ySol(nC, 0.0);
                    for (std::size_t c = 0; c < nC; ++c) {
                        if (!colUsed[c]) {
                            ySol[c] = std::round(static_cast<double>(w[intFree[cols[c]]]));
                        }
                    }
                    double maxFrac = 0.0;
                    std::size_t nPivots = 0;
                    std::vector<std::size_t> fracRows;
                    std::vector<char> pivotClean(nC, 0);
                    for (std::size_t rI = 0; rI < nR; ++rI) {
                        const std::size_t pc = pivotColOfRow[rI];
                        if (pc == kInvalidIndex) {
                            continue;
                        }
                        double val = rhs2[rI];
                        for (std::size_t c = 0; c < nC; ++c) {
                            if (c != pc && !colUsed[c] && A2[rI * nC + c] != 0.0) {
                                val -= A2[rI * nC + c] * ySol[c];
                            }
                        }
                        const double frac = std::abs(val - std::round(val));
                        maxFrac = std::max(maxFrac, frac);
                        if (frac >= 0.25) {
                            fracRows.push_back(rI);
                        }
                        ySol[pc] = std::round(val);
                        // A cleanly-determined, in-range pivot is injectable
                        // even when OTHER pivots came out fractional
                        // (variable-level partial pinning; the greedy
                        // schedule completes the rest).
                        pivotClean[pc] = frac < 0.25 && std::abs(ySol[pc]) <= tCap ? 1 : 0;
                        ++nPivots;
                    }
                    // A small number of fractional rows is local damage:
                    // drop those arcs and re-eliminate (their inconsistency
                    // smears across the whole component otherwise). A large
                    // number is the structural lattice residue — take the
                    // partial pinning as-is.
                    if (!fracRows.empty() && fracRows.size() <= std::max<std::size_t>(4, nR / 50) &&
                        round + 1 < kInjectRounds) {
                        for (const std::size_t rI : fracRows) {
                            if (injDrop[sys[rI].arc] == 0) {
                                injDrop[sys[rI].arc] = 1;
                                ++injDropped;
                            }
                        }
                        continue;  // rebuild without the inconsistent rows
                    }
                    std::size_t cleanPivots = 0;
                    for (std::size_t c = 0; c < nC; ++c) {
                        if (pivotClean[c]) {
                            ++cleanPivots;
                        }
                    }
                    const char* mode = std::getenv("CYBER_QC_BIMDF");
                    const bool inject = mode == nullptr || std::string(mode) != "report";
                    // Default injection requires FULL consistency: every
                    // pivot cleanly determined AND the T-mesh fully covered
                    // (no contained regions). The joint half-integer lattice
                    // leaves ~half the pivots at frac 0.5 on organic
                    // T-meshes, and pinning only a clean subset was measured
                    // to cost quality (nefertiti@4000 sing 396 -> 431); the
                    // same mismatch appears at containment boundaries even
                    // when every remaining pivot is clean (cylinder@1000
                    // haus 0.037 -> 0.067, box_sharp@1000 pure sing 19 ->
                    // 23). The pinned values assume flow values for the
                    // uncovered rest; greedy gives them something else.
                    // CYBER_QC_BIMDF=force enables partial pinning for
                    // experiments.
                    const bool force = mode != nullptr && std::string(mode) == "force";
                    const bool exact = nPivots > 0 && cleanPivots == nPivots && maxFrac < 0.25 &&
                                       exclArcs == 0 && injDropped == 0;
                    std::fprintf(stderr,
                                 "[qc] bimdf inject: arcs=%zu badArcs=%zu exclArcs=%zu "
                                 "injDropped=%zu rows=%zu cols=%zu pivots=%zu cleanPivots=%zu "
                                 "maxFrac=%.4f mode=%s\n",
                                 tmesh.arcs.size(), badArcs, exclArcs, injDropped, nR, nC, nPivots,
                                 cleanPivots, maxFrac,
                                 !inject ? "report"
                                 : force ? "force"
                                         : "inject");
                    if (inject && cleanPivots > 0 && (exact || force)) {
                        for (std::size_t c = 0; c < nC; ++c) {
                            // Pivots only when cleanly solved; non-pivot
                            // columns carry their greedy-default rounding.
                            const bool pin =
                                colUsed[c] ? pivotClean[c] != 0 : std::abs(ySol[c]) <= tCap;
                            if (pin) {
                                bimdfPins.push_back({cols[c], ySol[c]});
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    std::vector<char> intPinned(intFree.size(), 0);
    constexpr double kConfident = 0.2;
    std::size_t remaining = intFree.size();
    // Guided rounding: activate the attraction (unless an exact injection
    // already carries the full flow assignment) and re-solve so the greedy
    // schedule's first scan sees the attracted relaxed values. The bordered
    // direct engine factorized the UNATTRACTED operator, so guided rounds
    // fall back to the masked CG.
    if (!steerRows.empty() && bimdfPins.empty()) {
        steering = true;
        for (const SteerRow& sr : steerRows) {
            for (const auto& [ri, cf] : sr.a) {
                gSolve[ri] += static_cast<float>(sr.mu * cf * sr.len);
            }
        }
        if (useDirect) {
            direct.finalize(mask, w);
            useDirect = false;
        }
        maskedSolve(mask);
    }
    // Bi-MDF injection: pin the flow-determined integers in ONE batch (values
    // validated against tCap above, deliberately NOT clamped — a clamped
    // subset would break the flow's conservation) and re-solve once; the
    // greedy schedule below finishes any remaining free integers.
    if (!bimdfPins.empty()) {
        for (const auto& [k, val] : bimdfPins) {
            if (intPinned[k]) {
                continue;
            }
            w[intFree[k]] = static_cast<float>(val);
            mask[intFree[k]] = 0;
            intPinned[k] = 1;
            --remaining;
        }
        if (useDirect) {
            ++maskedSolveCalls;
            if (!direct.resolve(mask, w)) {
                direct.finalize(mask, w);
                useDirect = false;
                maskedSolve(mask);
            }
        } else {
            maskedSolve(mask);
        }
    }
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
            std::partial_sort(
                frac.begin(),
                frac.begin() + static_cast<std::ptrdiff_t>(std::min(batch, frac.size())),
                frac.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < batch && i < frac.size(); ++i) {
                pin(frac[i].second);
            }
        }
        if (useDirect) {
            ++maskedSolveCalls;
            if (!direct.resolve(mask, w)) {
                // Bordered factor lost positivity (should not happen on an SPD
                // reduced operator): recover the exact current solution and
                // finish on the historical masked CG.
                direct.finalize(mask, w);
                useDirect = false;
                maskedSolve(mask);
            }
        } else {
            maskedSolve(mask);
        }
    }
    if (useDirect) {
        direct.finalize(mask, w);
    }
    // Guided rounding: the integers now carry the flow structure; the final
    // continuous DOF must be pure Dirichlet, so drop the attraction and
    // re-solve once over the frozen integer assignment.
    if (steering) {
        steering = false;
        gSolve = gReduced;
        maskedSolve(mask);
    }

    // Reconstruct z = T w on the UV block.
    accel::Buffer<float> wBuf(w), zUv;
    accel::spmv(backend, Tuv, wBuf, zUv);
    for (std::size_t i = 0; i < nCut; ++i) {
        u[i] = zUv[i];
        v[i] = zUv[nCut + i];
    }

    if (dbg) {
        std::fprintf(stderr,
                     "[qc] reduced: nCut=%zu seams=%zu vars=%zu free=%zu intFree=%zu "
                     "maskedSolves=%d totalCg=%d\n",
                     nCut, nSeam, N, W, intFree.size(), maskedSolveCalls, totalCg);
        const double eFinal = reducedEnergy();
        std::fprintf(stderr, "[qc] reduced energy: relaxed=%.6f final=%.6f delta=%.6f\n", eRelaxed,
                     eFinal, eFinal - eRelaxed);
    }
    // Realized T-mesh deviation energy of whatever quantizer actually ran
    // (Bi-MDF injection or the greedy schedule, CYBER_QC_BIMDF=report): the
    // arc lengths the final integers produce vs the relaxed targets.
    if (bimdfTm && bimdfTm->ok && !bimdfArcRows.empty()) {
        std::vector<double> finalLen(bimdfTm->arcs.size(), 0.0);
        for (std::size_t a = 0; a < bimdfTm->arcs.size(); ++a) {
            if (bimdfTm->arcs[a].noExpr) {
                // Boundary-terminated arcs have no symbolic length; report
                // them neutrally (their realized deviation is unobservable).
                finalLen[a] = bimdfTm->arcs[a].len;
                continue;
            }
            double lenA = 0.0;
            for (const auto& [ri, cf] : bimdfArcRows[a]) {
                lenA += cf * static_cast<double>(w[ri]);
            }
            finalLen[a] = lenA;
        }
        std::fprintf(stderr, "[qc] bimdf realized: arcDeviationEnergy=%.3f injected=%zu\n",
                     bimdf::deviationEnergy(*bimdfTm, finalLen), bimdfPins.size());
    }
    return totalCg;
}

}  // namespace

namespace {

// Shared implementation of solveParameterization and relaxedCellArea. When
// `probeCellArea` is non-null the solve stops right after the initial relaxed
// Poisson solve and reports the UV grid-cell count there (no ARAP polish, no
// integer phase); the returned Parameterization stays invalid. A null pointer
// runs the full historical solve, unchanged.
Parameterization solveParameterizationImpl(const Mesh& mesh, const SeamlessSetup& setup,
                                           float spacing, accel::IBackend& backend,
                                           const CancelToken* cancel, double* probeCellArea,
                                           SeamlessSolveCache* cache) {
    Parameterization out;
    if (!setup.valid || spacing <= 0.0f || mesh.faceCapacity() == 0) {
        return out;
    }
    const float invS = 1.0f / spacing;
    // Feature-pinning lever: reconstruct combed targets on the comb's own angle() convention
    // (see combedDirection). Lever-off keeps the historical direction() branch bit-compatible.
    const bool angleConvention = setup.featureBinding;

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
    // Bi-MDF quantization context (CYBER_QC_BIMDF): compact faces, cone flags
    // and seam-side face indices for the motorcycle-graph tracer. Built only
    // when the opt-in flag is set; the default path is untouched.
    std::unique_ptr<bimdf::Charts> bimdfCharts;
    std::unordered_map<Index, std::size_t> faceToCompact;
    if (std::getenv("CYBER_QC_BIMDF") != nullptr && probeCellArea == nullptr) {
        bimdfCharts = std::make_unique<bimdf::Charts>();
        bimdfCharts->nCut = nCut;
        bimdfCharts->coneIndex.assign(nCut, 0);
        bimdfCharts->vertexOfCut.assign(nCut, 0);
    }
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> vs = mesh.faceVertices(f);
        const std::array<Vec3, 3> p{mesh.position(vs[0]), mesh.position(vs[1]),
                                    mesh.position(vs[2])};
        const Vec3 nrm = cross(p[1] - p[0], p[2] - p[0]);
        const float area2 = length(nrm);
        if (area2 < 1e-20f) {
            continue;
        }
        const Vec3 n = nrm / area2;
        FaceData fd;
        fd.area = 0.5f * area2;
        fd.e0 = normalized(combedDirection(setup.field, f, n, comb[fi], angleConvention));
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
        if (bimdfCharts) {
            faceToCompact.emplace(fi, faceData.size());
            for (int k = 0; k < 3; ++k) {
                const std::size_t cut = fd.cut[static_cast<std::size_t>(k)];
                const Index vv = vs[static_cast<std::size_t>(k)].value;
                bimdfCharts->vertexOfCut[cut] = static_cast<std::uint32_t>(vv);
                bimdfCharts->coneIndex[cut] = setup.singularityIndex[vv];
                if (bimdfCharts->vertexPos.size() <= vv) {
                    bimdfCharts->vertexPos.resize(vv + 1, {0.0f, 0.0f, 0.0f});
                }
                const Vec3 pp = mesh.position(vs[static_cast<std::size_t>(k)]);
                bimdfCharts->vertexPos[vv] = {pp.x, pp.y, pp.z};
            }
            bimdfCharts->faces.push_back(fd.cut);
            const Vec3 fn = cross(fd.e0, fd.e1);
            bimdfCharts->faceE0.push_back({fd.e0.x, fd.e0.y, fd.e0.z});
            bimdfCharts->faceN.push_back({fn.x, fn.y, fn.z});
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

    // Direct path (CYBER_QC_DIRECT, default on; docs/ROADMAP.md perf entry):
    // factor the pinned SPD operator ONCE — it is fixed across the initial
    // relaxed solve, every ARAP re-solve (only the RHS rotates) and every
    // calibration attempt/probe (spacing only scales the RHS) — and make each
    // relaxedSolve a pair of back-substitutions instead of two CG solves. The
    // factorization lands in the caller's cache when one is provided.
    // CYBER_QC_NO_DIRECT reverts to the historical CG byte-exactly.
    SeamlessSolveCacheImpl* cacheImpl = nullptr;
    if (cache != nullptr && std::getenv("CYBER_QC_NO_DIRECT") == nullptr) {
        if (!cache->impl) {
            cache->impl = std::make_shared<SeamlessSolveCacheImpl>();
        }
        cacheImpl = cache->impl.get();
    }
    SparseCholesky ownCholA;
    const SparseCholesky* cholA = nullptr;
    if (std::getenv("CYBER_QC_NO_DIRECT") == nullptr) {
        SparseCholesky* target = cacheImpl != nullptr ? &cacheImpl->cholA : &ownCholA;
        if (cacheImpl != nullptr && cacheImpl->aReady && cacheImpl->cholA.dim() == nCut) {
            cholA = target;
        } else {
            if (cacheImpl != nullptr) {
                cacheImpl->aReady = false;
            }
            const auto tA = std::chrono::steady_clock::now();
            const std::vector<double> aVal(A.value.begin(), A.value.end());
            if (target->factor(nCut, A.rowStart, A.colIndex, aVal)) {
                cholA = target;
                if (cacheImpl != nullptr) {
                    cacheImpl->aReady = true;
                }
                if (std::getenv("CYBER_QC_TIME") != nullptr) {
                    std::fprintf(stderr, "[qc-time] direct poisson: factorA=%ldms cholNnz=%zu\n",
                                 msSince(tA), target->factorNnz());
                }
            }
            // factor failure (non-SPD assembly) falls through to CG below
        }
    }

    std::vector<float> u(nCut, 0.0f), v(nCut, 0.0f);

    // CYBER_QC_FIELD_STATS diagnostics on the +z flat region (|n.z| > 0.99): combed-target
    // axis mix (fd.e0 vs world x̂/ŷ — a quarter-turn mix on ONE coplanar patch is the shear
    // smoking gun) and a per-phase histogram of grad(u)'s deviation from the nearest grid
    // axis (5-degree bins of min(ang mod 90, 90 - ang mod 90)).
    const bool fieldStats = std::getenv("CYBER_QC_FIELD_STATS") != nullptr;
    const auto statsAxisMix = [&]() {
        if (!fieldStats) {
            return;
        }
        // Split by side: +z (top) vs -z (bottom) so a per-patch mix is unambiguous.
        std::size_t topX = 0, topY = 0, botX = 0, botY = 0, nOther = 0;
        for (const FaceData& fd : faceData) {
            const Vec3 n = cross(fd.e0, fd.e1);
            if (std::fabs(n.z) <= 0.99f) {
                continue;
            }
            const bool top = n.z > 0.0f;
            if (std::fabs(fd.e0.x) > 0.9f) {
                (top ? topX : botX) += 1;
            } else if (std::fabs(fd.e0.y) > 0.9f) {
                (top ? topY : botY) += 1;
            } else {
                ++nOther;
            }
        }
        std::fprintf(stderr,
                     "[qc-stats] axis mix: top e0~x=%zu e0~y=%zu | bot e0~x=%zu e0~y=%zu | "
                     "other=%zu\n",
                     topX, topY, botX, botY, nOther);
        // Interior (non-feature) cut edges whose both faces are on a +-z flat side: any such
        // edge is a tree cut crossing a coplanar patch.
        std::size_t interiorCutFlat = 0, interiorCutTotal = 0;
        for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || !setup.isCutEdge[ei] ||
                mesh.isFeatureEdge(e)) {
                continue;
            }
            ++interiorCutTotal;
            const auto ef = mesh.edgeFaces(e);
            bool flat = true;
            for (const FaceId f : ef) {
                const std::vector<VertexId> vs = mesh.faceVertices(f);
                const Vec3 nn = cross(mesh.position(vs[1]) - mesh.position(vs[0]),
                                      mesh.position(vs[2]) - mesh.position(vs[0]));
                const float len = length(nn);
                if (len < 1e-20f || std::fabs(nn.z / len) <= 0.99f) {
                    flat = false;
                }
            }
            if (flat) {
                ++interiorCutFlat;
            }
        }
        std::fprintf(stderr, "[qc-stats] non-feature cut edges: total=%zu on +-z flats=%zu\n",
                     interiorCutTotal, interiorCutFlat);
    };
    const auto statsGradHist = [&](const char* phase) {
        if (!fieldStats) {
            return;
        }
        std::array<std::size_t, 9> bins{};
        std::size_t total = 0;
        for (const FaceData& fd : faceData) {
            const Vec3 n = cross(fd.e0, fd.e1);
            if (std::fabs(n.z) <= 0.99f) {
                continue;
            }
            Vec3 gradU{0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 3; ++k) {
                gradU = gradU + fd.gradPhi[static_cast<std::size_t>(k)] *
                                    u[fd.cut[static_cast<std::size_t>(k)]];
            }
            const float ang = std::atan2(gradU.y, gradU.x) * 180.0f / kPi;  // in the +z plane
            float m = std::fmod(std::fmod(ang, 90.0f) + 90.0f, 90.0f);
            m = std::min(m, 90.0f - m);  // deviation from the nearest grid axis, [0, 45]
            const std::size_t bin = std::min<std::size_t>(8, static_cast<std::size_t>(m / 5.0f));
            ++bins[bin];
            ++total;
        }
        std::fprintf(stderr, "[qc-stats] %s grad(u) dev hist (+z faces, 5-deg bins, n=%zu):", phase,
                     total);
        for (const std::size_t b : bins) {
            std::fprintf(stderr, " %zu", b);
        }
        std::fprintf(stderr, "\n");
    };

    const int maxIters = static_cast<int>(nCut) + 500;
    // Solve the relaxed (per-component pinned) Poisson for the current full RHS bu/bv. The
    // pin zeroes the RHS at the pinned/isolated vertices. Direct path: two exact
    // back-substitutions on the one-time factorization above. CG path: u,v are warm-started
    // from their incoming value so each ARAP re-solve (a small RHS perturbation) converges
    // fast.
    bool directCheckDone = false;
    const auto relaxedSolve = [&]() {
        std::vector<float> pbu = bu, pbv = bv;
        for (std::size_t i = 0; i < nCut; ++i) {
            if (isPin[i] || rows[i].empty()) {
                pbu[i] = 0.0f;
                pbv[i] = 0.0f;
            }
        }
        if (cholA != nullptr) {
            std::vector<double> rhs(nCut);
            std::vector<double> sol;
            for (std::size_t i = 0; i < nCut; ++i) {
                rhs[i] = static_cast<double>(pbu[i]);
            }
            cholA->solve(rhs, sol);
            for (std::size_t i = 0; i < nCut; ++i) {
                u[i] = static_cast<float>(sol[i]);
            }
            for (std::size_t i = 0; i < nCut; ++i) {
                rhs[i] = static_cast<double>(pbv[i]);
            }
            cholA->solve(rhs, sol);
            for (std::size_t i = 0; i < nCut; ++i) {
                v[i] = static_cast<float>(sol[i]);
            }
            out.cgIterationsU = 0;
            out.cgIterationsV = 0;
            // CYBER_QC_DIRECT_CHECK: numerical drift diagnostic — re-run the
            // first relaxed solve with the historical cold-started CG and
            // report max |uv_direct - uv_cg| (both approximate the same linear
            // system; the direct one is exact to double roundoff).
            if (!directCheckDone && std::getenv("CYBER_QC_DIRECT_CHECK") != nullptr) {
                directCheckDone = true;
                std::vector<float> uc(nCut, 0.0f), vc(nCut, 0.0f);
                conjugateGradient(backend, A, pbu, uc, maxIters, 1e-8f, cancel);
                conjugateGradient(backend, A, pbv, vc, maxIters, 1e-8f, cancel);
                double dmax = 0.0;
                for (std::size_t i = 0; i < nCut; ++i) {
                    dmax = std::max(dmax, static_cast<double>(std::fabs(u[i] - uc[i])));
                    dmax = std::max(dmax, static_cast<double>(std::fabs(v[i] - vc[i])));
                }
                std::fprintf(stderr, "[qc] direct check: max|uv_direct-uv_cg|=%.3e nCut=%zu\n",
                             dmax, nCut);
            }
            return;
        }
        out.cgIterationsU = conjugateGradient(backend, A, pbu, u, maxIters, 1e-8f, cancel);
        out.cgIterationsV = conjugateGradient(backend, A, pbv, v, maxIters, 1e-8f, cancel);
    };
    relaxedSolve();
    if (cancel != nullptr && cancel->isCancelled()) {
        return out;  // out.valid stays false -> caller degrades cleanly
    }

    // UV grid-cell count at the current u,v: sum of |UV triangle areas| (~1 extracted
    // quad candidate per unit cell). Serves the relaxed-only calibration probe and the
    // CYBER_QC_DEBUG probe-vs-full drift line below.
    const auto uvCellSum = [&]() {
        double cells = 0.0;
        for (const FaceData& fd : faceData) {
            const double ax = u[fd.cut[0]], ay = v[fd.cut[0]];
            const double bx = u[fd.cut[1]], by = v[fd.cut[1]];
            const double cx = u[fd.cut[2]], cy = v[fd.cut[2]];
            cells += 0.5 * std::fabs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
        }
        return cells;
    };
    if (probeCellArea != nullptr) {
        // Relaxed-only probe: the cell area is already fixed here (ARAP preserves the
        // target frame norm; integer rounding shifts sub-cell translations), so skip
        // the polish + integer phase. `out` stays invalid — probe callers only read
        // the cell count.
        *probeCellArea = uvCellSum();
        return out;
    }
    const bool debugCells = std::getenv("CYBER_QC_DEBUG") != nullptr;
    const double relaxedCells = debugCells ? uvCellSum() : 0.0;

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
    const bool arapEnabled = std::getenv("CYBER_QC_NO_ARAP") == nullptr;
    for (int iter = 0; arapEnabled && boundaryFrac < kBoundaryGate && iter < kArapIters; ++iter) {
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
    // the integer-seamless phase. bu/bv already hold the RHS for the final `angle` on every
    // path: each ARAP round assembles before it solves, and when the polish is skipped the
    // initial assembleRhs({}) is the angle==0 assembly — so no re-assembly is needed here.
    const std::vector<float> bu0 = bu, bv0 = bv;
    statsAxisMix();
    statsGradHist("relaxed");
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
        // Combed grid symmetry from side A (ef[0]) to side B (ef[1]). The combed frame of B
        // is the frame of A rotated intrinsically by Delta = p + comb[B] - comb[A] quarter
        // turns (p from edgeJump, both angles measured against the shared edge). A grid whose
        // frame rotates by +Delta has coordinates that rotate by -Delta: uv_B = R^{-Delta}
        // uv_A + t. The historical formula (comb[B] - comb[A] - p) has the comb difference
        // NEGATED — off by a half-turn whenever the difference is odd, which reverses the
        // along-crease coordinate on one side and makes the integer phase fight the relaxed
        // targets. Fixed under the feature-pinning lever; lever-off keeps the historical
        // formula bit-compatible.
        const int p = setup.periodJump[ei];
        const int combDiff = comb[ef[1].value] - comb[ef[0].value];
        const int rhoRaw = angleConvention ? -(p + combDiff) : (combDiff - p);
        s.rho = ((rhoRaw % 4) + 4) % 4;
        if (mesh.isFeatureEdge(e)) {
            s.feature = true;
            // The coordinate CONSTANT along the crease, in side A's combed
            // frame: the edge running along e0 means u varies along it, so v
            // is the constant coordinate (and vice versa). The cross field is
            // hard-pinned to feature edges, so the edge is near-parallel to
            // one frame axis and the choice is well-defined.
            const std::vector<VertexId> vsA = mesh.faceVertices(ef[0]);
            const Vec3 pa0 = mesh.position(vsA[0]);
            const Vec3 nrmA = cross(mesh.position(vsA[1]) - pa0, mesh.position(vsA[2]) - pa0);
            const float lenA = length(nrmA);
            if (lenA > 1e-20f) {
                const Vec3 nA = nrmA / lenA;
                const Vec3 e0 = normalized(
                    combedDirection(setup.field, ef[0], nA, comb[ef[0].value], angleConvention));
                const Vec3 e1 = cross(nA, e0);
                const Vec3 dir = normalized(mesh.position(b) - mesh.position(a));
                const float a0 = std::fabs(dot(dir, e0));
                const float a1 = std::fabs(dot(dir, e1));
                s.pinAxis = a0 >= a1 ? 1 : 0;
                // Pin only when the field actually runs along the crease
                // (within 30 degrees). Forcing a coordinate constant along an
                // edge the field crosses diagonally is M2d's measured false
                // premise: it buys nothing and costs severe grid distortion.
                if (std::max(a0, a1) < 0.866f) {
                    s.feature = false;
                }
            } else {
                s.feature = false;
            }
        }
        if (bimdfCharts) {
            const auto ia = faceToCompact.find(ef[0].value);
            const auto ib = faceToCompact.find(ef[1].value);
            if (ia == faceToCompact.end() || ib == faceToCompact.end()) {
                bimdfCharts.reset();  // degenerate seam face: charts incomplete
            } else {
                bimdf::SeamEdge se;
                se.aA = s.aA;
                se.bA = s.bA;
                se.aB = s.aB;
                se.bB = s.bB;
                se.rho = s.rho;
                se.feature = s.feature && std::getenv("CYBER_QC_NO_FEATURE_PIN") == nullptr;
                se.pinAxis = s.pinAxis;
                se.faceA = ia->second;
                se.faceB = ib->second;
                se.tx = 2 * nCut + 2 * bimdfCharts->seams.size();
                se.ty = se.tx + 1;
                bimdfCharts->seams.push_back(se);
            }
        }
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
        solveSeamlessReduced(backend, nCut, rows, bu0, bv0, seams, gauges, u, v, cancel, cacheImpl,
                             bimdfCharts.get());
    }
    statsGradHist("reduced");
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
    if (debugCells) {
        // Probe-vs-full drift: how far the relaxed-only cell count (what the calibration
        // probe measures) sits from the final one (what extraction actually sees).
        std::fprintf(stderr, "[qc] uv cells: relaxed=%.1f final=%.1f\n", relaxedCells, uvCellSum());
    }
    out.valid = true;
    return out;
}

}  // namespace

Parameterization solveParameterization(const Mesh& mesh, const SeamlessSetup& setup, float spacing,
                                       accel::IBackend& backend, const CancelToken* cancel,
                                       SeamlessSolveCache* cache) {
    return solveParameterizationImpl(mesh, setup, spacing, backend, cancel, nullptr, cache);
}

double relaxedCellArea(const Mesh& mesh, const SeamlessSetup& setup, float spacing,
                       accel::IBackend& backend, const CancelToken* cancel,
                       SeamlessSolveCache* cache) {
    double cells = -1.0;
    (void)solveParameterizationImpl(mesh, setup, spacing, backend, cancel, &cells, cache);
    return cells;
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
