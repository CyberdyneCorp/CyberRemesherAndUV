#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/build_tools.hpp"  // Affine
#include "cyber/retopo/neighbors.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/relax.hpp"
#include "cyber/retopo/snapping.hpp"

// Soft selection (manual-retopology spec, "Gradient region selection",
// "Selection operations", "Weighted transform with surface glue"): a per-vertex
// weight field in [0,1] over the EditMesh, filled from three region sources
// (line gradient, sphere, painted strokes), reshaped by the selection ops, and
// consumed by the weighted transform/relax.
//
// The weighted ops re-project onto the Target INSIDE the same pass: surface glue
// is part of the operation, never a separate cleanup call. A vertex whose weight
// is <= 0 is skipped entirely — it is neither moved nor re-snapped — which is
// what makes zero-weight vertices bit-identical after any weighted operation.
//
// Weights are indexed by raw vertex id, so any op that REASSIGNS ids (see the
// element-id stability note in cyber_capi.h — subdivide rebuilds the mesh)
// orphans a stored selection; callers drop it exactly where they drop their
// other id-keyed annotations. Header-only and inline, like the other actions.
namespace cyber::retopo {

// ---- weight container ------------------------------------------------------

// Per-vertex weights in [0,1], indexed by VertexId, sized lazily to the mesh's
// vertex capacity. Vertices past the stored end read as 0.
class SoftSelection {
public:
    // Grows the weight vector to cover every vertex slot of `mesh`. Existing
    // weights keep their value; new slots start at 0.
    void resizeFor(const Mesh& mesh) {
        const std::size_t needed = static_cast<std::size_t>(mesh.vertexCapacity());
        if (m_weights.size() < needed) {
            m_weights.resize(needed, 0.0f);
        }
    }

    [[nodiscard]] float weight(VertexId v) const {
        const std::size_t i = static_cast<std::size_t>(v.value);
        return i < m_weights.size() ? m_weights[i] : 0.0f;
    }

    // Stores `w` clamped to [0,1], growing the vector if needed.
    void setWeight(VertexId v, float w) {
        const std::size_t i = static_cast<std::size_t>(v.value);
        if (i >= m_weights.size()) {
            m_weights.resize(i + 1, 0.0f);
        }
        m_weights[i] = sanitize(w);
    }

    // Zeroes every weight, keeping the allocated size.
    void clear() { std::fill(m_weights.begin(), m_weights.end(), 0.0f); }

    [[nodiscard]] std::span<const float> weights() const { return m_weights; }
    [[nodiscard]] std::size_t size() const { return m_weights.size(); }

    // Replaces the whole field, clamping each incoming value into [0,1].
    void fromWeights(std::span<const float> w) {
        m_weights.assign(w.begin(), w.end());
        for (float& value : m_weights) {
            value = sanitize(value);
        }
    }

private:
    // std::clamp returns NaN unchanged (both of its comparisons are false for
    // NaN), so clamping alone does NOT enforce the [0,1] invariant. A NaN that
    // reaches the field then slips past the `w <= 0` early-out in the weighted
    // transforms and lerps a vertex to (nan,nan,nan) — one bad weight silently
    // destroys geometry. Non-finite input means "not selected".
    [[nodiscard]] static float sanitize(float w) {
        return std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    }

    std::vector<float> m_weights;
};

// ---- falloff curves --------------------------------------------------------

// Editable falloff shapes. All map 0 -> 0 and 1 -> 1 and are monotone, so the
// curve changes how a gradient feels without changing where it starts or ends.
enum class Falloff {
    Linear,  // t
    Smooth,  // smoothstep — the default gradient feel
    Sharp,   // t^2, weight builds late
    Round,   // quarter circle, weight builds early
};

[[nodiscard]] inline float applyFalloff(Falloff falloff, float t) {
    const float x = std::clamp(t, 0.0f, 1.0f);
    switch (falloff) {
        case Falloff::Linear:
            return x;
        case Falloff::Smooth:
            return x * x * (3.0f - 2.0f * x);
        case Falloff::Sharp:
            return x * x;
        case Falloff::Round:
            return std::sqrt(x * (2.0f - x));
    }
    return x;
}

// ---- line gradient ---------------------------------------------------------

// Quantizes `dir` to the nearest multiple of `degrees` measured IN THE VIEW
// PLANE — the plane through the origin perpendicular to `viewDir`, the same
// screen-space convention drawStripPath and surfaceCutSegment already use. The
// result lies in that plane and keeps the original length. A direction that is
// degenerate (zero, or parallel to the view direction) is returned unchanged.
[[nodiscard]] inline Vec3 snapDirectionToIncrement(Vec3 dir, Vec3 viewDir, float degrees) {
    const float step = degrees * 3.14159265358979323846f / 180.0f;
    if (step <= 0.0f || lengthSquared(dir) <= 0.0f || lengthSquared(viewDir) <= 0.0f) {
        return dir;
    }
    const Vec3 n = normalized(viewDir);
    const Vec3 helper = std::abs(n.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 u = normalized(helper - n * dot(helper, n));
    const Vec3 v = cross(n, u);
    const float du = dot(dir, u);
    const float dv = dot(dir, v);
    if (du * du + dv * dv <= 0.0f) {
        return dir;  // the line points straight at the viewer: no in-plane angle
    }
    const float snapped = std::round(std::atan2(dv, du) / step) * step;
    return (u * std::cos(snapped) + v * std::sin(snapped)) * length(dir);
}

// Line region: weight ramps 0 at `anchor` to 1 at `end` following `falloff`, and
// stays 1 beyond `end`. With `snapAngle` the anchor->end direction is quantized
// to `snapDegrees` increments in the view plane first.
struct LineRegion {
    Vec3 anchor{};
    Vec3 end{};
    Vec3 viewDir{0.0f, 0.0f, 1.0f};
    bool snapAngle = false;
    float snapDegrees = 15.0f;
    Falloff falloff = Falloff::Smooth;
};

// Replaces the selection with the line gradient. A degenerate line (anchor ==
// end) leaves the selection untouched.
inline void selectLine(const Mesh& mesh, SoftSelection& selection, const LineRegion& region) {
    Vec3 axis = region.end - region.anchor;
    if (region.snapAngle) {
        axis = snapDirectionToIncrement(axis, region.viewDir, region.snapDegrees);
    }
    const float axisLengthSquared = lengthSquared(axis);
    if (axisLengthSquared <= 0.0f) {
        return;
    }
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const float t = dot(mesh.position(v) - region.anchor, axis) / axisLengthSquared;
        selection.setWeight(v, applyFalloff(region.falloff, t));
    }
}

// ---- sphere region ---------------------------------------------------------

// Sphere region: weight 1 at `center` falling to 0 at `radius` along `falloff`.
struct SphereRegion {
    Vec3 center{};
    float radius = 1.0f;
    Falloff falloff = Falloff::Smooth;
};

// Replaces the selection with the radial falloff. A non-positive radius leaves
// the selection untouched.
inline void selectSphere(const Mesh& mesh, SoftSelection& selection, const SphereRegion& region) {
    if (region.radius <= 0.0f) {
        return;
    }
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const float d = length(mesh.position(v) - region.center);
        selection.setWeight(v, applyFalloff(region.falloff, 1.0f - d / region.radius));
    }
}

// ---- painted region --------------------------------------------------------

// Brush shared by every dab of one stroke.
struct PaintBrush {
    float radius = 1.0f;
    bool subtract = false;
    Falloff falloff = Falloff::Smooth;
};

// One brush sample along a stroke.
struct PaintDab {
    Vec3 center{};
    float pressure = 1.0f;
};

// Accumulates one dab into the selection: covered weights gain (or, in subtract
// mode, lose) pressure * falloff, saturating at 1 and bottoming out at 0.
inline void paintSelection(const Mesh& mesh, SoftSelection& selection, const PaintDab& dab,
                           const PaintBrush& brush) {
    if (brush.radius <= 0.0f) {
        return;
    }
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const float d = length(mesh.position(v) - dab.center);
        if (d >= brush.radius) {
            continue;
        }
        const float amount = dab.pressure * applyFalloff(brush.falloff, 1.0f - d / brush.radius);
        const float w = selection.weight(v);
        selection.setWeight(v, brush.subtract ? w - amount : w + amount);
    }
}

// Accumulates a whole stroke. Vertices outside the stroke's bounding box grown
// by the brush radius cannot be reached by any dab, so they are rejected once
// instead of once per sample.
inline void paintSelectionStroke(const Mesh& mesh, SoftSelection& selection,
                                 std::span<const PaintDab> dabs, const PaintBrush& brush) {
    if (dabs.empty() || brush.radius <= 0.0f) {
        return;
    }
    Vec3 lo = dabs.front().center;
    Vec3 hi = dabs.front().center;
    for (const PaintDab& dab : dabs) {
        lo = Vec3{std::min(lo.x, dab.center.x), std::min(lo.y, dab.center.y),
                  std::min(lo.z, dab.center.z)};
        hi = Vec3{std::max(hi.x, dab.center.x), std::max(hi.y, dab.center.y),
                  std::max(hi.z, dab.center.z)};
    }
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const Vec3 p = mesh.position(v);
        if (p.x < lo.x - brush.radius || p.x > hi.x + brush.radius || p.y < lo.y - brush.radius ||
            p.y > hi.y + brush.radius || p.z < lo.z - brush.radius || p.z > hi.z + brush.radius) {
            continue;
        }
        float w = selection.weight(v);
        for (const PaintDab& dab : dabs) {
            const float d = length(p - dab.center);
            if (d >= brush.radius) {
                continue;
            }
            const float amount =
                dab.pressure * applyFalloff(brush.falloff, 1.0f - d / brush.radius);
            w = std::clamp(brush.subtract ? w - amount : w + amount, 0.0f, 1.0f);
        }
        selection.setWeight(v, w);
    }
}

// ---- selection operations --------------------------------------------------

inline void clearSelection(SoftSelection& selection) { selection.clear(); }

// w -> 1 - w over the live vertices.
inline void invertSelection(const Mesh& mesh, SoftSelection& selection) {
    selection.resizeFor(mesh);
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v)) {
            selection.setWeight(v, 1.0f - selection.weight(v));
        }
    }
}

namespace detail {

// One double-buffered morphological/diffusion sweep over the closed one-ring.
// `combine(accumulator, neighbourWeight)` folds each neighbour into the running
// value seeded with the vertex's own weight; `finish(value, ringSize)` turns the
// accumulator into the new weight.
template <typename Combine, typename Finish>
inline void sweepSelection(const Mesh& mesh, SoftSelection& selection, int steps, Combine combine,
                           Finish finish) {
    if (steps <= 0) {
        return;
    }
    selection.resizeFor(mesh);
    std::vector<float> next(selection.weights().begin(), selection.weights().end());
    for (int step = 0; step < steps; ++step) {
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            const std::size_t slot = static_cast<std::size_t>(i);
            if (!mesh.isAlive(v)) {
                next[slot] = selection.weight(v);
                continue;
            }
            float value = selection.weight(v);
            const std::vector<VertexId> ring = oneRing(mesh, v);
            for (const VertexId nb : ring) {
                value = combine(value, selection.weight(nb));
            }
            next[slot] = finish(value, ring.size());
        }
        selection.fromWeights(next);
    }
}

}  // namespace detail

// Grows the selection: each vertex takes the maximum of its closed one-ring,
// repeated `steps` times.
inline void expandSelection(const Mesh& mesh, SoftSelection& selection, int steps = 1) {
    detail::sweepSelection(
        mesh, selection, steps, [](float a, float b) { return std::max(a, b); },
        [](float value, std::size_t) { return value; });
}

// Shrinks the selection: each vertex takes the minimum of its closed one-ring,
// repeated `steps` times.
inline void contractSelection(const Mesh& mesh, SoftSelection& selection, int steps = 1) {
    detail::sweepSelection(
        mesh, selection, steps, [](float a, float b) { return std::min(a, b); },
        [](float value, std::size_t) { return value; });
}

// Softens the weight transition: each vertex takes the average of its closed
// one-ring, repeated `steps` times (the shell's "smooth by 1 / 5 / 10" is just
// this count). Positions are never touched.
inline void smoothSelection(const Mesh& mesh, SoftSelection& selection, int steps = 1) {
    detail::sweepSelection(
        mesh, selection, steps, [](float a, float b) { return a + b; },
        [](float value, std::size_t ring) { return value / static_cast<float>(ring + 1); });
}

// ---- weighted transform / relax --------------------------------------------

// Applies `xf` per vertex scaled by the selection weight, then re-projects onto
// the Target in the SAME pass when a snapper is supplied — the surface glue the
// spec requires, not a separate snap-all afterwards. Vertices with weight <= 0
// and pinned vertices are skipped entirely, so their positions are bit-identical
// afterwards. `resnapEpsilon` is the distance below which a re-projection is not
// worth reporting.
inline ResnapReport transformWeighted(Mesh& mesh, const SoftSelection& selection, const Affine& xf,
                                      const SurfaceSnapper* snap = nullptr,
                                      const PinSet* pins = nullptr, float resnapEpsilon = 0.0f) {
    ResnapReport report;
    const bool snapping = snap != nullptr && !snap->empty();
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const float w = selection.weight(v);
        if (w <= 0.0f || (pins != nullptr && pins->isPinned(v))) {
            continue;
        }
        const Vec3 pos = mesh.position(v);
        const Vec3 target = lerp(pos, xf.apply(pos), w);
        ++report.moved;
        if (!snapping) {
            mesh.setPosition(v, target);
            continue;
        }
        const Vec3 glued = snap->snapToSurface(target).point;
        const float pulled = length(glued - target);
        if (pulled > resnapEpsilon) {
            ++report.resnapped;
            report.maxSnapDistance = std::max(report.maxSnapDistance, pulled);
        }
        mesh.setPosition(v, glued);
    }
    return report;
}

// Weighted relax: the ordinary relax sweep (auto-pinned corners, brush mask,
// tangential projection, in-pass re-snap) with the per-vertex blend additionally
// scaled by the selection weight. Zero-weight vertices are untouched.
inline ResnapReport relaxWeighted(Mesh& mesh, const SoftSelection& selection,
                                  const RelaxParams& params, const SurfaceSnapper* snap = nullptr,
                                  const PinSet* pins = nullptr, float resnapEpsilon = 0.0f) {
    return detail::relaxSweep(mesh, params, pins, snap, resnapEpsilon,
                              [&selection](VertexId v) { return selection.weight(v); });
}

}  // namespace cyber::retopo
