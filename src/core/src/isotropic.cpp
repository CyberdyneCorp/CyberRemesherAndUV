#include "cyber/core/isotropic.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <utility>
#include <vector>

namespace cyber::remesh {

namespace {

constexpr float kLongFactor = 4.0f / 3.0f;
constexpr float kShortFactor = 4.0f / 5.0f;
// Cancellation poll granularity: cheap enough to keep worst-case latency
// far under the spec's 100 ms.
constexpr std::size_t kCancelStride = 4096;
constexpr float kMinScale = 0.3f;
constexpr float kMaxScale = 3.0f;

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

// Gradation limiting (standard sizing-field practice): the local target may
// grow by at most kGradation * targetEdgeLength per unit of geodesic
// distance from a finer region. Keeps refinement local to the curvature that
// demands it — without it, one high-valence vertex (a cap-fan center whose
// neighbors are all on a fine rim) inherits the fine scale in a single
// smoothing hop and blankets an entire flat region.
constexpr float kGradation = 0.5f;

void limitGradation(const Mesh& mesh, float targetEdgeLength, std::vector<float>& scales) {
    // Multi-source Dijkstra: every vertex is a source at its own scale;
    // relax scale_j <= scale_i + kGradation * |edge ij| / targetEdgeLength.
    using Entry = std::pair<float, Index>;  // (scale, vertex), min-heap
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{i})) {
            queue.emplace(scales[i], i);
        }
    }
    while (!queue.empty()) {
        const auto [scale, i] = queue.top();
        queue.pop();
        if (scale > scales[i]) {
            continue;  // stale entry
        }
        const VertexId v{i};
        for (const EdgeId e : mesh.vertexEdges(v)) {
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = a == v ? b : a;
            const float step =
                kGradation * length(mesh.position(other) - mesh.position(v)) / targetEdgeLength;
            const float allowed = scale + step;
            if (allowed < scales[other.value]) {
                scales[other.value] = allowed;
                queue.emplace(allowed, other.value);
            }
        }
    }
}

// Per-vertex target scaling from discrete curvature (angle defect per area),
// AutoRemesher-style: scale = clamp(normalizedCurvature^(-adaptivity),
// kMinScale, kMaxScale) so flat regions get longer edges, then gradation-
// limited. adaptivity 0 -> all 1 (remeshing-parameters spec, "Adaptivity
// zero is uniform").
void computeTargetScales(const Mesh& mesh, float adaptivity, float targetEdgeLength,
                         std::vector<float>& scales) {
    scales.assign(mesh.vertexCapacity(), 1.0f);
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

    // Vertices on feature edges (creases, boundaries, non-manifold edges) are
    // excluded from the curvature source: their angle defect measures the
    // crease itself, which the feature constraints already preserve exactly —
    // it says nothing about the surface detail that adaptivity exists to
    // resolve. Feeding it in blanketed entire flat regions with the floor
    // scale (a capped cylinder's fan cap ended up all-0.3: the crease defect
    // smoothed across the disc through the fan center) and inflated the
    // normalization mean, compressing genuinely curved regions to the coarse
    // clamp. Feature vertices get their scale from nearby smooth regions via
    // the gradation pass at the end.
    auto onFeatureEdge = [&mesh](VertexId v) {
        for (const EdgeId e : mesh.vertexEdges(v)) {
            if (mesh.isFeatureEdge(e)) {
                return true;
            }
        }
        return false;
    };
    double totalAbsDefect = 0.0;
    std::size_t includedCount = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v) && area[i] > 0.0f && !onFeatureEdge(v)) {
            const float defect = std::fabs(2.0f * kPi - angleSum[i]);
            curvature[i] = defect / area[i];
            totalAbsDefect += defect;
            ++includedCount;
        }
    }

    // Mean-relative normalization amplifies whatever variation exists — on a
    // surface whose smooth regions are (near-)developable the defect field is
    // float noise, and normalizing it would paint random scales. Only adapt
    // when the total defect clears the accumulated-noise floor (~1e-5 rad per
    // vertex; real curvature on a closed surface totals >= 4*pi*|chi|).
    if (totalAbsDefect < std::fmax(1e-3, 1e-5 * static_cast<double>(includedCount))) {
        return;
    }

    // Normalize by the mean over the included (non-feature) vertices so a
    // typical vertex gets scale 1 (a uniform-curvature surface stays
    // uniform); flatter-than-average regions get longer edges,
    // higher-curvature regions shorter, clamped to [kMinScale, kMaxScale].
    double meanCurvature = 0.0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        meanCurvature += curvature[i];
    }
    meanCurvature /= static_cast<double>(includedCount);
    if (meanCurvature <= 0.0) {
        return;
    }
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        if (!mesh.isAlive(VertexId{i})) {
            continue;
        }
        if (curvature[i] <= 0.0f) {
            scales[i] = kMaxScale;  // flat or feature vertex: coarsest allowed
            continue;
        }
        const float normalizedCurvature =
            std::fmax(curvature[i] / static_cast<float>(meanCurvature), 1e-3f);
        scales[i] = std::clamp(std::pow(normalizedCurvature, -adaptivity), kMinScale, kMaxScale);
    }

    // Feature vertices and flat regions start at the coarse clamp; the
    // gradation pass pulls them down near genuinely fine regions so
    // refinement decays at a bounded rate instead of jumping.
    limitGradation(mesh, targetEdgeLength, scales);
}

// Barycentric weights of p projected into the triangle, clamped inside it.
std::array<float, 3> barycentricWeights(Vec3 p, const std::array<Vec3, 3>& tri) {
    const Vec3 v0 = tri[1] - tri[0];
    const Vec3 v1 = tri[2] - tri[0];
    const Vec3 v2 = p - tri[0];
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-20f) {
        return {1.0f, 0.0f, 0.0f};  // degenerate input triangle
    }
    const float v = std::clamp((d11 * d20 - d01 * d21) / denom, 0.0f, 1.0f);
    const float w = std::clamp((d00 * d21 - d01 * d20) / denom, 0.0f, 1.0f - v);
    return {1.0f - v - w, v, w};
}

// Eulerian adaptive-scale field: per-vertex scales are computed ONCE from the
// input geometry, snapshotted per input triangle, and evaluated at arbitrary
// points by closest-point query against the (immutable) reference surface.
//
// History of this design: scales were first recomputed per iteration (mean
// renormalization compounded refinement — 100x face explosion, see the
// runaway regression test), then carried as a vertex attribute interpolated
// through splits. The attribute was Lagrangian: tangential smoothing
// physically migrates vertices, dragging fine rim scales across flat regions
// where they trigger fresh splits every iteration — a second unbounded
// explosion (25k+ quads on a 600-quad capped cylinder, caught by the
// benchmark harness). Sampling the field from the fixed input surface removes
// the feedback path entirely: a point's target depends only on where it lies
// on the reference, never on mesh evolution.
class ScaleField {
public:
    ScaleField(const Mesh& inputMesh, const ReferenceSurface& reference, float adaptivity,
               float targetEdgeLength)
        : m_reference(reference), m_uniform(adaptivity <= 0.0f) {
        if (m_uniform) {
            return;
        }
        std::vector<float> vertexScales;
        computeTargetScales(inputMesh, adaptivity, targetEdgeLength, vertexScales);

        m_triangles.resize(inputMesh.faceCapacity());
        for (Index fi = 0; fi < inputMesh.faceCapacity(); ++fi) {
            const FaceId f{fi};
            if (!inputMesh.isAlive(f)) {
                continue;
            }
            // isotropicRemesh's contract requires triangulated input.
            const auto verts = inputMesh.faceVertices(f);
            FieldTriangle& tri = m_triangles[fi];
            for (std::size_t k = 0; k < 3; ++k) {
                tri.position[k] = inputMesh.position(verts[k]);
                tri.scale[k] = vertexScales[verts[k].value];
            }
        }
    }

    [[nodiscard]] bool uniform() const { return m_uniform; }

    // Scale at an arbitrary point: barycentric interpolation on the closest
    // input triangle. One BVH query per call — the passes bound-check first
    // so mid-band edges skip the query entirely.
    [[nodiscard]] float at(Vec3 p) const {
        if (m_uniform) {
            return 1.0f;
        }
        const Bvh::ClosestHit hit = m_reference.closest(p);
        if (!hit.face.valid() || hit.face.value >= m_triangles.size()) {
            return 1.0f;
        }
        const FieldTriangle& tri = m_triangles[hit.face.value];
        const std::array<float, 3> w = barycentricWeights(hit.point, tri.position);
        const float scale = w[0] * tri.scale[0] + w[1] * tri.scale[1] + w[2] * tri.scale[2];
        return std::clamp(scale, kMinScale, kMaxScale);
    }

private:
    struct FieldTriangle {
        std::array<Vec3, 3> position{};
        std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    };

    const ReferenceSurface& m_reference;
    std::vector<FieldTriangle> m_triangles;
    bool m_uniform = false;
};

class SplitPass : public Pass {
public:
    using Pass::Pass;

    // Splits every edge longer than 4/3 of its local target (the field
    // sampled at the edge midpoint); each adjacent face (which becomes a quad
    // after the edge split) is re-triangulated by connecting the midpoint to
    // its opposite corner.
    void run(const ScaleField& field, const CancelToken* cancel) {
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
            const float len = edgeLength(e);
            // No scale in [kMinScale, kMaxScale] could make this edge long —
            // skip the field query (a BVH lookup) entirely.
            if (len <= m_options.targetEdgeLength * kMinScale * kLongFactor) {
                continue;
            }
            const Vec3 mid = lerp(m_mesh.position(a), m_mesh.position(b), 0.5f);
            const float target = m_options.targetEdgeLength * field.at(mid);
            if (len <= target * kLongFactor) {
                continue;
            }
            // Feature edges: never split into sub-band halves. The halves inherit the
            // feature flag and their endpoints become feature vertices, which the
            // collapse pass refuses to touch — so halves below the collapse band are
            // PERMANENT over-density pinned to the crease. At target densities where
            // 4/3*target < len < 2*kShortFactor*target (the input crease segmentation
            // "crossing" the solve density) this shed sliver cascades over whole flat
            // patches: measured on the generated cylinder at ~2500 quads, the work mesh
            // blew up 4.9k -> 13.6k faces with 1295 sub-1-degree cap slivers, whose junk
            // normals re-tagged ~900 spurious feature seams and made the seamless solve
            // diverge. Splitting only when both halves stay collapsible-band-or-longer
            // keeps the crease geometry exact and the density bounded.
            if (m_mesh.isFeatureEdge(e) && len < 2.0f * target * kShortFactor) {
                continue;
            }
            const VertexId midVertex = m_mesh.splitEdge(e, 0.5f);
            if (!midVertex.valid()) {
                continue;
            }
            // Re-triangulate the (now quad) incident faces.
            for (const FaceId f : m_mesh.vertexFaces(midVertex)) {
                if (m_mesh.faceSize(f) > 3) {
                    retriangulate(f, midVertex);
                }
            }
        }
    }

private:
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

    void run(const ScaleField& field, const CancelToken* cancel) {
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
            const float len = edgeLength(e);
            // No scale in [kMinScale, kMaxScale] could make this edge short —
            // skip the field query (a BVH lookup) entirely.
            if (len >= m_options.targetEdgeLength * kMaxScale * kShortFactor) {
                continue;
            }
            const Vec3 mid = lerp(m_mesh.position(a), m_mesh.position(b), 0.5f);
            if (len >= m_options.targetEdgeLength * field.at(mid) * kShortFactor) {
                continue;
            }
            // Feature vertices are never collapsed (spec).
            if (isFeatureVertex(a) || isFeatureVertex(b)) {
                continue;
            }
            if (!collapseKeepsBandedLengths(a, b, mid, field)) {
                continue;
            }
            m_mesh.collapseEdge(e);
        }
    }

private:
    // Classic guard: collapsing to the midpoint must not create edges longer
    // than the allowed band, or collapses fight the split pass forever. Each
    // surviving edge is judged against ITS OWN local target (the field at
    // that edge's post-collapse midpoint), not the collapsing edge's: with
    // adaptive scales the collapsing edge can sit in a fine region while a
    // neighbor legitimately reaches into a coarse one — judging that
    // neighbor by the fine target vetoes the collapse and deadlocks sliver
    // clusters into leftover density.
    bool collapseKeepsBandedLengths(VertexId a, VertexId b, Vec3 mid,
                                    const ScaleField& field) const {
        for (const VertexId endpoint : {a, b}) {
            for (const EdgeId incident : m_mesh.vertexEdges(endpoint)) {
                const auto [x, y] = m_mesh.edgeVertices(incident);
                const VertexId other = x == endpoint ? y : x;
                if (other == a || other == b) {
                    continue;
                }
                const Vec3 otherPos = m_mesh.position(other);
                const float target =
                    m_options.targetEdgeLength * field.at(lerp(otherPos, mid, 0.5f));
                if (length(otherPos - mid) > target * kLongFactor) {
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

    // One immutable scale field for the whole run, sampled from the input
    // surface (see ScaleField) — mesh evolution can never feed back into
    // targets.
    const bool isoTime = std::getenv("CYBER_ISO_TIME") != nullptr;
    using Clk = std::chrono::steady_clock;
    const auto t0 = Clk::now();
    const ScaleField field(mesh, reference, options.adaptivity, options.targetEdgeLength);
    long long tScale = 0, tSplit = 0, tCollapse = 0, tFlip = 0, tSmooth = 0;
    const auto ms = [](Clk::time_point a, Clk::time_point b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    if (isoTime) {
        tScale = ms(t0, Clk::now());
    }
    auto finish = [&](IsotropicStatus status) {
        if (isoTime) {
            std::fprintf(
                stderr,
                "[iso-time] iters=%d faces=%zu | scales=%lldms split=%lldms collapse=%lldms "
                "flip=%lldms smooth+project=%lldms\n",
                options.iterations, mesh.faceCount(), tScale, tSplit, tCollapse, tFlip, tSmooth);
        }
        return status;
    };

    // Convergence-triggered extension: the fixed iteration budget assumes the input is
    // within a few split levels of the target density. A far-coarser input (the generated
    // cylinder's 49-gon fan caps at ~4500 quads: 1.2k -> 52k faces in the first split
    // pass) is still mid-transient after `options.iterations` rounds — the ping-pong
    // between split and collapse leaves 2-3x over-density and thousands of degenerate
    // slivers whose junk normals then poison downstream feature tagging. Keep iterating
    // while the split pass is still growing the mesh materially (> kSplitGrowthConverged),
    // up to a hard cap. Meshes that converged inside the budget run the exact same
    // iterations as before (the extension never triggers), so equilibrium outputs are
    // byte-identical.
    constexpr float kSplitGrowthConverged = 1.10f;
    const int maxIterations = options.iterations + 7;
    bool splitStillGrowing = true;
    for (int iteration = 0;
         iteration < maxIterations && (iteration < options.iterations || splitStillGrowing);
         ++iteration) {
        auto a = Clk::now();
        const std::size_t facesBeforeSplit = mesh.faceCount();
        SplitPass(mesh, options).run(field, cancel);
        splitStillGrowing = static_cast<float>(mesh.faceCount()) >
                            kSplitGrowthConverged * static_cast<float>(facesBeforeSplit);
        if (isoTime) {
            tSplit += ms(a, Clk::now());
            a = Clk::now();
        }
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        CollapsePass(mesh, options).run(field, cancel);
        if (isoTime) {
            tCollapse += ms(a, Clk::now());
            a = Clk::now();
        }
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        FlipPass(mesh, options).run(cancel);
        if (isoTime) {
            tFlip += ms(a, Clk::now());
            a = Clk::now();
        }
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        SmoothAndProjectPass(mesh, options, reference).run(cancel);
        if (isoTime) {
            tSmooth += ms(a, Clk::now());
        }
        if (cancel && cancel->isCancelled()) {
            return finish(IsotropicStatus::Cancelled);
        }
        if (progress) {
            progress->report(std::min(1.0f, static_cast<float>(iteration + 1) /
                                                static_cast<float>(options.iterations)),
                             "isotropic");
        }
    }
    return finish(IsotropicStatus::Success);
}

}  // namespace cyber::remesh
