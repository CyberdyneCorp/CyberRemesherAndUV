#include "cyber/core/isotropic.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cyber::remesh {

namespace {

constexpr float kLongFactor = 4.0f / 3.0f;
constexpr float kShortFactor = 4.0f / 5.0f;
// Cancellation poll granularity: cheap enough to keep worst-case latency
// far under the spec's 100 ms.
constexpr std::size_t kCancelStride = 4096;
// Adaptive target scales are computed ONCE from the input geometry and
// carried as a vertex attribute so the kernel interpolates them through
// splits and collapses. Recomputing per iteration compounds refinement
// (above-average-curvature vertices re-normalize below 1 every round) and
// runs away — observed as a 100x face-count explosion.
constexpr const char* kScaleAttribute = "__isotropicTargetScale";

class Pass {
public:
    Pass(Mesh& mesh, const IsotropicOptions& options) : m_mesh(mesh), m_options(options) {}

protected:
    [[nodiscard]] bool isFeatureVertex(VertexId v) const {
        for (const EdgeId e : m_mesh.vertexEdges(v)) {
            if (m_mesh.isFeatureEdge(e)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] float edgeLength(EdgeId e) const {
        const auto [a, b] = m_mesh.edgeVertices(e);
        return length(m_mesh.position(a) - m_mesh.position(b));
    }

    Mesh& m_mesh;
    const IsotropicOptions& m_options;
};

// Per-vertex target scaling from discrete curvature (angle defect per area),
// AutoRemesher-style: scale = clamp(normalizedCurvature^(-adaptivity),
// 0.3, 3.0) so flat regions get longer edges. adaptivity 0 -> all 1
// (remeshing-parameters spec, "Adaptivity zero is uniform").
void computeTargetScales(Mesh& mesh, float adaptivity, std::vector<float>& scales) {
    std::fill(scales.begin(), scales.end(), 1.0f);
    if (adaptivity <= 0.0f) {
        return;
    }

    std::vector<float> curvature(mesh.vertexCapacity(), 0.0f);
    std::vector<float> area(mesh.vertexCapacity(), 0.0f);
    std::vector<float> angleSum(mesh.vertexCapacity(), 0.0f);

    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);
        const std::size_t n = verts.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3 p = mesh.position(verts[i]);
            const Vec3 prev = mesh.position(verts[(i + n - 1) % n]);
            const Vec3 next = mesh.position(verts[(i + 1) % n]);
            const Vec3 u = normalized(prev - p);
            const Vec3 w = normalized(next - p);
            angleSum[verts[i].value] += std::acos(std::clamp(dot(u, w), -1.0f, 1.0f));
            area[verts[i].value] += length(cross(next - p, prev - p)) / 6.0f;
        }
    }

    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{i}) && area[i] > 0.0f) {
            curvature[i] = std::fabs(2.0f * kPi - angleSum[i]) / area[i];
        }
    }

    // The raw angle-defect estimator is noisy; two Laplacian smoothing
    // passes keep the adaptive field from amplifying tessellation artifacts.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> smoothed = curvature;
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v)) {
                continue;
            }
            float sum = curvature[i];
            std::size_t count = 1;
            for (const EdgeId e : mesh.vertexEdges(v)) {
                const auto [a, b] = mesh.edgeVertices(e);
                sum += curvature[(a == v ? b : a).value];
                ++count;
            }
            smoothed[i] = sum / static_cast<float>(count);
        }
        curvature = std::move(smoothed);
    }

    // Normalize by the mean so a typical vertex gets scale 1 (a uniform-
    // curvature surface stays uniform); flatter-than-average regions get
    // longer edges, high-curvature regions shorter, clamped to [0.3, 3.0].
    double meanCurvature = 0.0;
    std::size_t aliveCount = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{i})) {
            meanCurvature += curvature[i];
            ++aliveCount;
        }
    }
    if (aliveCount == 0 || meanCurvature <= 0.0) {
        return;
    }
    meanCurvature /= static_cast<double>(aliveCount);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (!mesh.isAlive(VertexId{i})) {
            continue;
        }
        const float normalizedCurvature =
            std::fmax(curvature[i] / static_cast<float>(meanCurvature), 1e-3f);
        scales[i] = std::clamp(std::pow(normalizedCurvature, -adaptivity), 0.3f, 3.0f);
    }
}

class SplitPass : public Pass {
public:
    using Pass::Pass;

    // Splits every edge longer than 4/3 of its local target; each adjacent
    // face (which becomes a quad after the edge split) is re-triangulated by
    // connecting the midpoint to its opposite corner.
    void run(const std::vector<float>& scales, const CancelToken* cancel) {
        // New edges appear at the end of the index space; one sweep over the
        // starting capacity plus growth converges for this pass's purpose.
        for (Index i = 0; i < m_mesh.edgeCapacity(); ++i) {
            if (cancel && i % kCancelStride == 0 && cancel->isCancelled()) {
                return;
            }
            const EdgeId e{i};
            if (!m_mesh.isAlive(e)) {
                continue;
            }
            const auto [a, b] = m_mesh.edgeVertices(e);
            const float target =
                m_options.targetEdgeLength * 0.5f * (scaleOf(scales, a) + scaleOf(scales, b));
            if (edgeLength(e) <= target * kLongFactor) {
                continue;
            }
            const VertexId mid = m_mesh.splitEdge(e, 0.5f);
            if (!mid.valid()) {
                continue;
            }
            // Re-triangulate the (now quad) incident faces.
            for (const FaceId f : m_mesh.vertexFaces(mid)) {
                if (m_mesh.faceSize(f) > 3) {
                    retriangulate(f, mid);
                }
            }
        }
    }

private:
    static float scaleOf(const std::vector<float>& scales, VertexId v) {
        return v.value < scales.size() ? scales[v.value] : 1.0f;
    }

    void retriangulate(FaceId f, VertexId mid) {
        const auto verts = m_mesh.faceVertices(f);
        const std::size_t n = verts.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (verts[i] == mid) {
                const VertexId opposite = verts[(i + 2) % n];
                m_mesh.splitFace(f, mid, opposite);
                return;
            }
        }
    }
};

class CollapsePass : public Pass {
public:
    using Pass::Pass;

    void run(const std::vector<float>& scales, const CancelToken* cancel) {
        const Index initialCapacity = static_cast<Index>(m_mesh.edgeCapacity());
        for (Index i = 0; i < initialCapacity; ++i) {
            if (cancel && i % kCancelStride == 0 && cancel->isCancelled()) {
                return;
            }
            const EdgeId e{i};
            if (!m_mesh.isAlive(e)) {
                continue;
            }
            const auto [a, b] = m_mesh.edgeVertices(e);
            const float scaleA = a.value < scales.size() ? scales[a.value] : 1.0f;
            const float scaleB = b.value < scales.size() ? scales[b.value] : 1.0f;
            const float target = m_options.targetEdgeLength * 0.5f * (scaleA + scaleB);
            if (edgeLength(e) >= target * kShortFactor) {
                continue;
            }
            // Feature vertices are never collapsed (spec).
            if (isFeatureVertex(a) || isFeatureVertex(b)) {
                continue;
            }
            if (!collapseKeepsBandedLengths(e, a, b, target)) {
                continue;
            }
            m_mesh.collapseEdge(e);
        }
    }

private:
    // Classic guard: collapsing to the midpoint must not create edges longer
    // than the allowed band, or collapses fight the split pass forever.
    bool collapseKeepsBandedLengths(EdgeId e, VertexId a, VertexId b, float target) const {
        const Vec3 mid = lerp(m_mesh.position(a), m_mesh.position(b), 0.5f);
        for (const VertexId endpoint : {a, b}) {
            for (const EdgeId incident : m_mesh.vertexEdges(endpoint)) {
                if (incident == e) {
                    continue;
                }
                const auto [x, y] = m_mesh.edgeVertices(incident);
                const VertexId other = x == endpoint ? y : x;
                if (length(m_mesh.position(other) - mid) > target * kLongFactor) {
                    return false;
                }
            }
        }
        return true;
    }
};

class FlipPass : public Pass {
public:
    using Pass::Pass;

    void run(const CancelToken* cancel) {
        const Index capacity = static_cast<Index>(m_mesh.edgeCapacity());
        for (Index i = 0; i < capacity; ++i) {
            if (cancel && i % kCancelStride == 0 && cancel->isCancelled()) {
                return;
            }
            const EdgeId e{i};
            // Feature edges are never flipped (spec: flips must not cross
            // feature edges — AutoRemesher's guard was commented out).
            if (!m_mesh.isAlive(e) || m_mesh.isFeatureEdge(e)) {
                continue;
            }
            if (m_mesh.edgeFaceCount(e) != 2) {
                continue;
            }
            if (valenceGain(e) > 0) {
                m_mesh.flipEdge(e);
            }
        }
    }

private:
    [[nodiscard]] int targetValence(VertexId v) const {
        return isFeatureVertex(v) ? 4 : 6;  // boundary/feature vertices aim lower
    }

    [[nodiscard]] int valenceGain(EdgeId e) const {
        const auto faces = m_mesh.edgeFaces(e);
        if (faces.size() != 2 || m_mesh.faceSize(faces[0]) != 3 || m_mesh.faceSize(faces[1]) != 3) {
            return 0;
        }
        const auto [a, b] = m_mesh.edgeVertices(e);
        VertexId c, d;
        for (const VertexId v : m_mesh.faceVertices(faces[0])) {
            if (!(v == a) && !(v == b)) {
                c = v;
            }
        }
        for (const VertexId v : m_mesh.faceVertices(faces[1])) {
            if (!(v == a) && !(v == b)) {
                d = v;
            }
        }
        if (!c.valid() || !d.valid() || c == d || m_mesh.edgeBetween(c, d).valid()) {
            return 0;
        }
        auto deviation = [this](VertexId v, int delta) {
            const int valence = static_cast<int>(m_mesh.vertexEdges(v).size()) + delta;
            return std::abs(valence - targetValence(v));
        };
        const int before = deviation(a, 0) + deviation(b, 0) + deviation(c, 0) + deviation(d, 0);
        const int after = deviation(a, -1) + deviation(b, -1) + deviation(c, +1) + deviation(d, +1);
        return before - after;
    }
};

class SmoothAndProjectPass : public Pass {
public:
    SmoothAndProjectPass(Mesh& mesh, const IsotropicOptions& options,
                         const ReferenceSurface& reference)
        : Pass(mesh, options), m_reference(reference) {}

    void run(const CancelToken* cancel) {
        // Tangential relaxation toward the neighbor centroid, then projection
        // back to the reference surface. Feature vertices stay untouched.
        std::vector<Vec3> newPositions(m_mesh.vertexCapacity());
        std::vector<bool> move(m_mesh.vertexCapacity(), false);

        for (Index i = 0; i < m_mesh.vertexCapacity(); ++i) {
            if (cancel && i % kCancelStride == 0 && cancel->isCancelled()) {
                return;
            }
            const VertexId v{i};
            if (!m_mesh.isAlive(v) || isFeatureVertex(v)) {
                continue;
            }
            const auto edges = m_mesh.vertexEdges(v);
            if (edges.empty()) {
                continue;
            }
            Vec3 centroid{};
            for (const EdgeId e : edges) {
                const auto [a, b] = m_mesh.edgeVertices(e);
                centroid += m_mesh.position(a == v ? b : a);
            }
            centroid = centroid / static_cast<float>(edges.size());

            const Vec3 p = m_mesh.position(v);
            const Vec3 normal = vertexNormal(v);
            Vec3 delta = (centroid - p) * 0.5f;
            delta = delta - normal * dot(delta, normal);  // tangential component
            newPositions[i] = p + delta;
            move[i] = true;
        }
        for (Index i = 0; i < m_mesh.vertexCapacity(); ++i) {
            if (!move[i]) {
                continue;
            }
            m_mesh.setPosition(VertexId{i}, m_reference.project(newPositions[i]));
        }
    }

private:
    [[nodiscard]] Vec3 vertexNormal(VertexId v) const {
        Vec3 n{};
        for (const FaceId f : m_mesh.vertexFaces(v)) {
            n += m_mesh.faceNormal(f);
        }
        return normalized(n);
    }

    const ReferenceSurface& m_reference;
};

}  // namespace

IsotropicStatus isotropicRemesh(Mesh& mesh, const ReferenceSurface& reference,
                                const IsotropicOptions& options, ProgressSink* progress,
                                const CancelToken* cancel) {
    if (options.targetEdgeLength <= 0.0f || !std::isfinite(options.targetEdgeLength) ||
        options.iterations <= 0 || reference.empty()) {
        return IsotropicStatus::InvalidInput;
    }

    // One scale field for the whole run, carried as a vertex attribute so
    // splits interpolate it and collapses keep the survivor's value.
    std::vector<float>& scales = mesh.vertexAttributes().create<float>(kScaleAttribute);
    computeTargetScales(mesh, options.adaptivity, scales);
    auto finish = [&mesh](IsotropicStatus status) {
        mesh.vertexAttributes().remove(kScaleAttribute);
        return status;
    };

    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        SplitPass(mesh, options).run(scales, cancel);
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        CollapsePass(mesh, options).run(scales, cancel);
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        FlipPass(mesh, options).run(cancel);
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        SmoothAndProjectPass(mesh, options, reference).run(cancel);
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        if (progress) {
            progress->report(
                static_cast<float>(iteration + 1) / static_cast<float>(options.iterations),
                "isotropic");
        }
    }
    return finish(IsotropicStatus::Success);
}

}  // namespace cyber::remesh
