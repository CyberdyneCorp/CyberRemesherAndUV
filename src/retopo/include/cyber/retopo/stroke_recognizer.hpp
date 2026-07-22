#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"

// Stroke recognizer (manual-retopology spec, "Pencil stroke grammar"):
// classifies a captured stroke (polyline of surface points) into one of the
// pencil actions. This is an explicitly BEST-EFFORT heuristic classifier —
// tolerant of imperfect strokes, deterministic, and unit-testable, but not a
// learned model. It only *labels* the stroke; the geometric edit itself is done
// by the real operations in actions.hpp. Header-only and inline.
namespace cyber::retopo {

enum class StrokeAction {
    None,             // matched nothing (spec wants non-intrusive feedback)
    CreateQuad,       // closed ~4-corner shape
    CreateTri,        // closed ~3-corner shape
    InsertLoop,       // open drag across faces
    Delete,           // scribble / X over geometry
    Merge,            // straight line between two vertices
    RotateEdge,       // small closed circle over an edge
    ExtrudeCylinder,  // large closed round loop
    Tweak,            // tiny / ambiguous stroke
};

struct StrokeResult {
    StrokeAction action = StrokeAction::None;
    float confidence = 0.0f;  // 0..1 heuristic score
};

struct StrokeParams {
    float closedFraction = 0.2f;    // endpoint gap < this * path length => closed
    float cornerRadians = 1.0f;     // turn sharper than this counts as a corner
    float straightRatio = 0.85f;    // endpointDist/pathLength above this => straight
    float rotateRadius = 0.5f;      // closed round loop below this radius => RotateEdge
    float minStrokeLength = 1e-4f;  // shorter strokes are treated as taps (Tweak)
};

namespace detail {

struct StrokeStats {
    float pathLength = 0.0f;
    float endpointDistance = 0.0f;
    float boundingRadius = 0.0f;
    int corners = 0;
    int selfIntersections = 0;
    bool closed = false;
};

// Drops the coordinate axis with the smallest spread, projecting to 2D so the
// heuristics work regardless of the stroke's 3D orientation.
inline std::vector<Vec2> projectTo2D(std::span<const Vec3> pts) {
    Vec3 lo = pts[0];
    Vec3 hi = pts[0];
    for (const Vec3 p : pts) {
        lo = min(lo, p);
        hi = max(hi, p);
    }
    const Vec3 extent = hi - lo;
    std::vector<Vec2> out;
    out.reserve(pts.size());
    if (extent.z <= extent.x && extent.z <= extent.y) {
        for (const Vec3 p : pts) out.push_back({p.x, p.y});
    } else if (extent.y <= extent.x && extent.y <= extent.z) {
        for (const Vec3 p : pts) out.push_back({p.x, p.z});
    } else {
        for (const Vec3 p : pts) out.push_back({p.y, p.z});
    }
    return out;
}

// Orientation sign of the ordered triple (a, b, c).
inline float orient(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const float d1 = orient(a, b, c);
    const float d2 = orient(a, b, d);
    const float d3 = orient(c, d, a);
    const float d4 = orient(c, d, b);
    return ((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f));
}

inline int countSelfIntersections(const std::vector<Vec2>& p) {
    int hits = 0;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        for (std::size_t j = i + 2; j + 1 < n; ++j) {
            if (i == 0 && j + 2 == n) {
                continue;  // first and last segments may legitimately share the seam
            }
            if (segmentsCross(p[i], p[i + 1], p[j], p[j + 1])) {
                ++hits;
            }
        }
    }
    return hits;
}

inline StrokeStats analyze(std::span<const Vec3> pts, const StrokeParams& params) {
    StrokeStats s;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        s.pathLength += length(pts[i] - pts[i - 1]);
    }
    s.endpointDistance = length(pts.back() - pts.front());
    // Count corners between consecutive non-degenerate segment directions.
    // Skipping zero-length segments (from duplicate/coincident samples) by
    // direction rather than by index keeps a real corner whose apex vertex was
    // sampled twice: its turn still registers between the last real incoming
    // direction and the next real outgoing one, instead of a phantom acos(0)=90°.
    bool havePrevDir = false;
    Vec3 prevDir{};
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Vec3 seg = pts[i] - pts[i - 1];
        if (lengthSquared(seg) < 1e-12f) {
            continue;  // coincident samples carry no direction
        }
        const Vec3 dir = normalized(seg);
        if (havePrevDir) {
            const float angle = std::acos(std::clamp(dot(prevDir, dir), -1.0f, 1.0f));
            if (angle > params.cornerRadians) {
                ++s.corners;
            }
        }
        prevDir = dir;
        havePrevDir = true;
    }
    Vec3 centroid{};
    for (const Vec3 p : pts) centroid += p;
    centroid = centroid * (1.0f / static_cast<float>(pts.size()));
    for (const Vec3 p : pts) s.boundingRadius = std::fmax(s.boundingRadius, length(p - centroid));

    s.closed = s.pathLength > 0.0f && s.endpointDistance < params.closedFraction * s.pathLength;
    const std::vector<Vec2> flat = projectTo2D(pts);
    s.selfIntersections = countSelfIntersections(flat);
    return s;
}

inline StrokeResult classifyClosed(const StrokeStats& s, const StrokeParams& params) {
    // A closed loop does not register a corner at its seam (the join between
    // the last and first points), so the polygon's side count is one more than
    // the detected interior corners: a square yields 3 corners, a triangle 2.
    const int sides = s.corners + 1;
    if (sides == 3) {
        return {StrokeAction::CreateTri, 0.8f};
    }
    if (sides >= 4 && sides <= 6) {
        return {StrokeAction::CreateQuad, 0.8f};
    }
    if (s.boundingRadius < params.rotateRadius) {
        return {StrokeAction::RotateEdge, 0.6f};
    }
    return {StrokeAction::ExtrudeCylinder, 0.6f};
}

inline StrokeResult classifyOpen(const StrokeStats& s, const StrokeParams& params) {
    if (s.selfIntersections >= 1 && s.corners >= 1) {
        return {StrokeAction::Delete, 0.8f};
    }
    const float straightness = s.pathLength > 0.0f ? s.endpointDistance / s.pathLength : 0.0f;
    if (straightness >= params.straightRatio) {
        return {StrokeAction::Merge, straightness};
    }
    return {StrokeAction::InsertLoop, 0.5f};
}

}  // namespace detail

[[nodiscard]] inline StrokeResult recognizeStroke(std::span<const Vec3> points,
                                                  const StrokeParams& params = {}) {
    if (points.size() < 3) {
        return {StrokeAction::Tweak, 0.4f};
    }
    const detail::StrokeStats s = detail::analyze(points, params);
    if (s.pathLength < params.minStrokeLength) {
        return {StrokeAction::Tweak, 0.4f};
    }
    return s.closed ? detail::classifyClosed(s, params) : detail::classifyOpen(s, params);
}

}  // namespace cyber::retopo
