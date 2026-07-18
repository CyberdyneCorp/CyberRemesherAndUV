#include "cyber/quadrangulate/field_quadrangulator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/quadrangulate/crossfield.hpp"

namespace cyber::remesh {

namespace {

constexpr std::size_t kCancelStride = 4096;

VertexId opposite(const Mesh& mesh, FaceId f, VertexId a, VertexId b) {
    for (const VertexId v : mesh.faceVertices(f)) {
        if (!(v == a) && !(v == b)) {
            return v;
        }
    }
    return {};
}

// Planarity x corner-squareness of the quad formed by merging (a,b,c)+(b,a,d)
// (matches the greedy baseline's metric so geometry quality is comparable).
float quadQuality(const Mesh& mesh, VertexId a, VertexId b, VertexId c, VertexId d) {
    const Vec3 pa = mesh.position(a), pd = mesh.position(d), pb = mesh.position(b),
               pc = mesh.position(c);
    const Vec3 n0 = normalized(cross(pb - pa, pc - pa));
    const Vec3 n1 = normalized(cross(pd - pa, pb - pa));
    const float planarity = std::fmax(0.0f, dot(n0, n1));
    const std::array<Vec3, 4> corners{pa, pd, pb, pc};
    float squareness = 1.0f;
    for (int i = 0; i < 4; ++i) {
        const Vec3 p = corners[static_cast<std::size_t>(i)];
        const Vec3 prev = corners[static_cast<std::size_t>((i + 3) % 4)];
        const Vec3 next = corners[static_cast<std::size_t>((i + 1) % 4)];
        const float angle =
            std::acos(std::clamp(dot(normalized(prev - p), normalized(next - p)), -1.0f, 1.0f));
        squareness = std::fmin(squareness, 1.0f - std::fabs(angle - kPi / 2.0f) / (kPi / 2.0f));
    }
    return planarity * std::fmax(0.0f, squareness);
}

// How diagonal edge direction `de` is to a cross direction `c` (both ~in-plane
// unit vectors), folded into the cross's 90-deg symmetry: 1 when the edge sits
// at 45 deg (a good quad diagonal to remove), 0 when it aligns with a field
// axis (a quad edge we should keep).
float diagonalness(Vec3 de, Vec3 c) {
    const float ang = std::acos(std::clamp(std::fabs(dot(de, c)), 0.0f, 1.0f));  // [0, 90] deg
    const float folded = std::fmin(ang, kPi / 2.0f - ang);                       // [0, 45] deg
    return folded / (kPi / 4.0f);
}

void mergePair(Mesh& mesh, EdgeId e, FaceId f0, FaceId f1) {
    const auto [a, b] = mesh.edgeVertices(e);
    VertexId u = a, w = b;
    for (const LoopId l : mesh.faceLoops(f0)) {
        if (mesh.loopVertex(l) == b && mesh.loopVertex(mesh.loopNext(l)) == a) {
            u = b;
            w = a;
            break;
        }
    }
    const VertexId c = opposite(mesh, f0, a, b);
    const VertexId d = opposite(mesh, f1, a, b);
    mesh.removeFace(f0);
    mesh.removeFace(f1);
    mesh.addFace(std::array{u, d, w, c});
}

// Dissolves interior valence-2 vertices (doublets): a vertex shared by exactly
// two quads and no boundary/feature edge, whose two quads merge into one. This
// is the quad-graph simplification of task 5.5. Returns how many were removed.
std::size_t removeDoublets(Mesh& mesh) {
    std::size_t removed = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const auto edges = mesh.vertexEdges(v);
        const auto faces = mesh.vertexFaces(v);
        if (edges.size() != 2 || faces.size() != 2) {
            continue;
        }
        bool boundaryOrFeature = false;
        for (const EdgeId e : edges) {
            if (mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
                boundaryOrFeature = true;
            }
        }
        const FaceId f0 = faces[0], f1 = faces[1];
        if (boundaryOrFeature || mesh.faceSize(f0) != 4 || mesh.faceSize(f1) != 4) {
            continue;
        }
        // f0 = (.. p, v, q ..): the neighbours of v in f0 are p and q, its
        // opposite corner is r; f1's opposite corner is s. Merged quad: r p s q.
        const auto ring = [&mesh, v](FaceId f, VertexId& prev, VertexId& next, VertexId& opp) {
            const std::vector<VertexId> fv = mesh.faceVertices(f);
            const std::size_t n = fv.size();
            for (std::size_t k = 0; k < n; ++k) {
                if (fv[k] == v) {
                    prev = fv[(k + n - 1) % n];
                    next = fv[(k + 1) % n];
                    opp = fv[(k + 2) % n];
                    return;
                }
            }
        };
        VertexId p0, q0, r, p1, q1, s;
        ring(f0, p0, q0, r);
        ring(f1, p1, q1, s);
        mesh.removeFace(f0);
        mesh.removeFace(f1);
        // f0 traverses p0 -> v -> q0 -> r; the merged quad walks the outer loop
        // r -> p0 -> s -> q0 (skipping v), keeping f0's orientation.
        if (mesh.addFace(std::array{r, p0, s, q0}).valid()) {
            mesh.removeIsolatedVertex(v);
            ++removed;
        }
    }
    return removed;
}

// ---- valence cleanup (roadmap 5.4/5.5) -------------------------------------

int valence(const Mesh& mesh, VertexId v) {
    return static_cast<int>(mesh.vertexEdges(v).size());
}

int deviation(int val) {
    const int d = val - 4;
    return d < 0 ? -d : d;
}

// A vertex is eligible for valence editing only when it is fully interior
// (every incident edge is a manifold, non-feature edge) and quad-only (every
// incident face is a quad). Editing such a vertex can never invalidate a
// boundary, feature or non-quad neighbourhood, and it always belongs to the
// "interior quad-only" set the metric measures.
bool eligibleVertex(const Mesh& mesh, VertexId v) {
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
            return false;
        }
    }
    for (const FaceId f : mesh.vertexFaces(v)) {
        if (mesh.faceSize(f) != 4) {
            return false;
        }
    }
    return true;
}

// The two quads sharing an interior edge, unrolled into an oriented hexagon.
// f0 walks v0 -> v1 -> p -> q; f1 (opposite orientation on the shared edge)
// walks v1 -> v0 -> r -> s. Removing the shared diagonal v0-v1 leaves the
// hexagon [v1, p, q, v0, r, s]; a rotation re-splits it with the diagonal p-r
// (Rot::PR) or q-s (Rot::QS), so both faces remain quads.
struct QuadPair {
    VertexId v0, v1, p, q, r, s;
};
enum class Rot { PR, QS };

VertexId cyclic(const std::vector<VertexId>& v, int k) {
    return v[static_cast<std::size_t>(((k % 4) + 4) % 4)];
}

bool extractQuadPair(const Mesh& mesh, EdgeId e, FaceId f0, FaceId f1, QuadPair& out) {
    const std::vector<VertexId> a = mesh.faceVertices(f0);
    const std::vector<VertexId> b = mesh.faceVertices(f1);
    if (a.size() != 4 || b.size() != 4) {
        return false;
    }
    const auto [ea, eb] = mesh.edgeVertices(e);
    int i0 = -1;
    for (int k = 0; k < 4; ++k) {
        const VertexId x = cyclic(a, k), y = cyclic(a, k + 1);
        if ((x == ea && y == eb) || (x == eb && y == ea)) {
            i0 = k;
            break;
        }
    }
    if (i0 < 0) {
        return false;
    }
    out.v0 = cyclic(a, i0);
    out.v1 = cyclic(a, i0 + 1);
    out.p = cyclic(a, i0 + 2);
    out.q = cyclic(a, i0 + 3);
    int j0 = -1;
    for (int k = 0; k < 4; ++k) {
        if (cyclic(b, k) == out.v1 && cyclic(b, k + 1) == out.v0) {
            j0 = k;
            break;
        }
    }
    if (j0 < 0) {
        return false;
    }
    out.r = cyclic(b, j0 + 2);
    out.s = cyclic(b, j0 + 3);
    return true;
}

bool sixDistinct(const QuadPair& qp) {
    const std::array<VertexId, 6> v{qp.v0, qp.v1, qp.p, qp.q, qp.r, qp.s};
    for (std::size_t i = 0; i < v.size(); ++i) {
        for (std::size_t j = i + 1; j < v.size(); ++j) {
            if (v[i] == v[j]) {
                return false;
            }
        }
    }
    return true;
}

// Best-effort field alignment hint (tie-break only, never affects validity): 1
// when the prospective new edge sits on a field axis, 0 when it is diagonal.
// The field is indexed by original face id, so it is guarded and only consulted
// opportunistically.
float fieldAlignment(const Mesh& mesh, VertexId m0, VertexId m1, FaceId f0, FaceId f1,
                     const CrossField* field) {
    if (field == nullptr || f0.value >= field->size() || f1.value >= field->size()) {
        return 0.0f;
    }
    const Vec3 d = normalized(mesh.position(m1) - mesh.position(m0));
    const float a0 = 1.0f - diagonalness(d, field->direction(f0));
    const float a1 = 1.0f - diagonalness(d, field->direction(f1));
    return 0.5f * (a0 + a1);
}

struct FlipChoice {
    bool apply = false;
    Rot rot = Rot::PR;
    int improvement = 0;
    float align = 0.0f;
};

// Picks the strictly-improving rotation (if any) for one quad pair. Both shared
// endpoints always lose a valence; the chosen receiving pair gains one each.
FlipChoice evaluate(const Mesh& mesh, const QuadPair& qp, FaceId f0, FaceId f1,
                    const CrossField* field) {
    FlipChoice best;
    const int shOld = deviation(valence(mesh, qp.v0)) + deviation(valence(mesh, qp.v1));
    const int shNew = deviation(valence(mesh, qp.v0) - 1) + deviation(valence(mesh, qp.v1) - 1);
    const auto consider = [&](Rot rot, VertexId m0, VertexId m1) {
        if (!eligibleVertex(mesh, m0) || !eligibleVertex(mesh, m1)) {
            return;
        }
        const int oldC = shOld + deviation(valence(mesh, m0)) + deviation(valence(mesh, m1));
        const int newC = shNew + deviation(valence(mesh, m0) + 1) + deviation(valence(mesh, m1) + 1);
        const int imp = oldC - newC;
        if (imp <= 0) {
            return;
        }
        const float al = fieldAlignment(mesh, m0, m1, f0, f1, field);
        if (imp > best.improvement || (imp == best.improvement && al > best.align + 1e-6f)) {
            best = {true, rot, imp, al};
        }
    };
    consider(Rot::PR, qp.p, qp.r);
    consider(Rot::QS, qp.q, qp.s);
    return best;
}

// Applies a rotation by removing the two quads and re-adding the two rotated
// quads over the same hexagon with a different internal diagonal. Fully guarded:
// bails (leaving the mesh untouched) if the new diagonal already exists or the
// shared edge is no longer a clean 2-quad edge. All corner vertices are distinct
// and alive, so both addFace calls succeed and face/quad counts are preserved.
bool applyRotation(Mesh& mesh, const QuadPair& qp, Rot rot) {
    const VertexId n0 = (rot == Rot::PR) ? qp.p : qp.q;
    const VertexId n1 = (rot == Rot::PR) ? qp.r : qp.s;
    if (mesh.edgeBetween(n0, n1).valid()) {
        return false;
    }
    const EdgeId shared = mesh.edgeBetween(qp.v0, qp.v1);
    if (!shared.valid() || mesh.edgeFaceCount(shared) != 2) {
        return false;
    }
    const auto faces = mesh.edgeFaces(shared);
    if (mesh.faceSize(faces[0]) != 4 || mesh.faceSize(faces[1]) != 4) {
        return false;
    }
    mesh.removeFace(faces[0]);
    mesh.removeFace(faces[1]);
    FaceId g0, g1;
    if (rot == Rot::PR) {
        g0 = mesh.addFace(std::array{qp.p, qp.q, qp.v0, qp.r});
        g1 = mesh.addFace(std::array{qp.r, qp.s, qp.v1, qp.p});
    } else {
        g0 = mesh.addFace(std::array{qp.q, qp.v0, qp.r, qp.s});
        g1 = mesh.addFace(std::array{qp.s, qp.v1, qp.p, qp.q});
    }
    return g0.valid() && g1.valid();
}

class FieldAlignedQuadrangulator final : public IQuadrangulator {
public:
    explicit FieldAlignedQuadrangulator(int iterations) : m_iterations(iterations) {}

    Outcome quadrangulate(Mesh& mesh, float /*targetEdgeLength*/, ProgressSink* progress,
                          const CancelToken* cancel) override {
        auto backend = accel::defaultBackend();
        const CrossField field = computeCrossField(mesh, m_iterations, *backend);

        struct Candidate {
            float score;
            Index edge;
        };
        std::vector<Candidate> candidates;
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            if (cancel && i % kCancelStride == 0 && cancel->isCancelled()) {
                return {.success = false, .cancelled = true, .failureReason = "cancelled"};
            }
            const EdgeId e{i};
            if (!mesh.isAlive(e) || mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
                continue;
            }
            const auto faces = mesh.edgeFaces(e);
            if (mesh.faceSize(faces[0]) != 3 || mesh.faceSize(faces[1]) != 3) {
                continue;
            }
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId c = opposite(mesh, faces[0], a, b);
            const VertexId d = opposite(mesh, faces[1], a, b);
            if (!c.valid() || !d.valid() || c == d) {
                continue;
            }
            const float quality = quadQuality(mesh, a, b, c, d);
            if (quality <= 0.2f) {
                continue;
            }
            const Vec3 de = normalized(mesh.position(b) - mesh.position(a));
            const float diag = 0.5f * (diagonalness(de, field.direction(faces[0])) +
                                       diagonalness(de, field.direction(faces[1])));
            // Weight geometry and field alignment; a diagonal edge scores best.
            candidates.push_back({quality * (0.25f + 0.75f * diag), i});
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
            if (x.score != y.score) {
                return x.score > y.score;
            }
            return x.edge < y.edge;
        });

        const std::size_t total = candidates.size();
        for (std::size_t k = 0; k < total; ++k) {
            if (cancel && k % kCancelStride == 0 && cancel->isCancelled()) {
                return {.success = false, .cancelled = true, .failureReason = "cancelled"};
            }
            const EdgeId e{candidates[k].edge};
            if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2) {
                continue;
            }
            const auto faces = mesh.edgeFaces(e);
            if (mesh.faceSize(faces[0]) != 3 || mesh.faceSize(faces[1]) != 3) {
                continue;
            }
            mergePair(mesh, e, faces[0], faces[1]);
            if (progress && k % kCancelStride == 0) {
                progress->report(static_cast<float>(k) / static_cast<float>(total), "quadrangulate");
            }
        }

        removeDoublets(mesh);          // graph simplification (5.5)
        quadValenceCleanup(mesh, &field);  // valence cleanup (5.4/5.5)
        if (progress) {
            progress->report(1.0f, "quadrangulate");
        }
        return {.success = true, .cancelled = false, .failureReason = {}};
    }

    [[nodiscard]] std::string name() const override { return "field-aligned"; }

private:
    int m_iterations;
};

}  // namespace

std::size_t quadValenceCleanup(Mesh& mesh, const CrossField* field, int maxPasses) {
    std::size_t applied = 0;
    for (int pass = 0; pass < maxPasses; ++pass) {
        std::size_t passApplied = 0;
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!mesh.isAlive(e) || mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
                continue;
            }
            const auto faces = mesh.edgeFaces(e);
            if (mesh.faceSize(faces[0]) != 4 || mesh.faceSize(faces[1]) != 4) {
                continue;
            }
            QuadPair qp;
            if (!extractQuadPair(mesh, e, faces[0], faces[1], qp) || !sixDistinct(qp)) {
                continue;
            }
            if (!eligibleVertex(mesh, qp.v0) || !eligibleVertex(mesh, qp.v1)) {
                continue;
            }
            const FlipChoice choice = evaluate(mesh, qp, faces[0], faces[1], field);
            if (choice.apply && applyRotation(mesh, qp, choice.rot)) {
                ++passApplied;
            }
        }
        applied += passApplied;
        if (passApplied == 0) {
            break;  // fixed point reached
        }
    }
    return applied;
}

std::unique_ptr<IQuadrangulator> makeFieldAlignedQuadrangulator(int fieldIterations) {
    return std::make_unique<FieldAlignedQuadrangulator>(fieldIterations);
}

}  // namespace cyber::remesh
