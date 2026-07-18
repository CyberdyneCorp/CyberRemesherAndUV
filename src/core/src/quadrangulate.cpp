#include "cyber/core/quadrangulate.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cyber::remesh {

namespace {

constexpr std::size_t kCancelStride = 4096;

// Quality of the quad formed by removing the shared edge (a,b) between the
// triangles (a,b,c) and (b,a,d): 1 is a perfect planar square, 0 unusable.
float quadQuality(const Mesh& mesh, VertexId a, VertexId b, VertexId c, VertexId d) {
    // Quad cycle: a -> d -> b -> c.
    const Vec3 pa = mesh.position(a), pd = mesh.position(d), pb = mesh.position(b),
               pc = mesh.position(c);

    // Planarity: angle between the two triangle normals.
    const Vec3 n0 = normalized(cross(pb - pa, pc - pa));
    const Vec3 n1 = normalized(cross(pd - pa, pb - pa));
    const float planarity = std::fmax(0.0f, dot(n0, n1));

    // Corner-angle squareness: each interior angle scored against 90 deg.
    const Vec3 corners[4] = {pa, pd, pb, pc};
    float squareness = 1.0f;
    for (int i = 0; i < 4; ++i) {
        const Vec3 p = corners[i];
        const Vec3 prev = corners[(i + 3) % 4];
        const Vec3 next = corners[(i + 1) % 4];
        const float angle =
            std::acos(std::clamp(dot(normalized(prev - p), normalized(next - p)), -1.0f, 1.0f));
        squareness = std::fmin(squareness, 1.0f - std::fabs(angle - kPi / 2.0f) / (kPi / 2.0f));
    }
    return planarity * std::fmax(0.0f, squareness);
}

class GreedyPairing final : public IQuadrangulator {
public:
    Outcome quadrangulate(Mesh& mesh, float /*targetEdgeLength*/, ProgressSink* progress,
                          const CancelToken* cancel) override {
        struct Candidate {
            float quality;
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
            if (quality > 0.2f) {
                candidates.push_back({quality, i});
            }
        }

        // Best quads first; ties broken by edge index for determinism.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
            if (x.quality != y.quality) {
                return x.quality > y.quality;
            }
            return x.edge < y.edge;
        });

        std::size_t merged = 0;
        const std::size_t total = candidates.size();
        for (std::size_t k = 0; k < total; ++k) {
            if (cancel && k % kCancelStride == 0 && cancel->isCancelled()) {
                return {.success = false, .cancelled = true, .failureReason = "cancelled"};
            }
            const EdgeId e{candidates[k].edge};
            if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2) {
                continue;  // a neighbor merge consumed one of the triangles
            }
            const auto faces = mesh.edgeFaces(e);
            if (mesh.faceSize(faces[0]) != 3 || mesh.faceSize(faces[1]) != 3) {
                continue;
            }
            mergePair(mesh, e, faces[0], faces[1]);
            ++merged;
            if (progress && k % kCancelStride == 0) {
                progress->report(static_cast<float>(k) / static_cast<float>(total),
                                 "quadrangulate");
            }
        }
        if (progress) {
            progress->report(1.0f, "quadrangulate");
        }
        (void)merged;
        return {.success = true, .cancelled = false, .failureReason = {}};
    }

    [[nodiscard]] std::string name() const override { return "greedy-pairing"; }

private:
    static VertexId opposite(const Mesh& mesh, FaceId f, VertexId a, VertexId b) {
        for (const VertexId v : mesh.faceVertices(f)) {
            if (!(v == a) && !(v == b)) {
                return v;
            }
        }
        return {};
    }

    // Replaces the two triangles with one quad, keeping winding from f0.
    static void mergePair(Mesh& mesh, EdgeId e, FaceId f0, FaceId f1) {
        const auto [a, b] = mesh.edgeVertices(e);
        // Orient by f0's traversal of the shared edge.
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
        // f0 was (u, w, c); insert d between u and w: (u, d, w, c).
        mesh.addFace(std::array{u, d, w, c});
    }
};

}  // namespace

std::unique_ptr<IQuadrangulator> makeGreedyPairingQuadrangulator() {
    return std::make_unique<GreedyPairing>();
}

}  // namespace cyber::remesh
