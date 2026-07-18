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

        removeDoublets(mesh);  // graph simplification (5.5)
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

std::unique_ptr<IQuadrangulator> makeFieldAlignedQuadrangulator(int fieldIterations) {
    return std::make_unique<FieldAlignedQuadrangulator>(fieldIterations);
}

}  // namespace cyber::remesh
