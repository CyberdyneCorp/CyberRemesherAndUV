#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/loops.hpp"

// Two-stage stroke interpreter (manual-retopology spec, "Pencil stroke
// grammar"; app design D5 "Contextual gesture grammar"):
//
//   Stage 1 — classifyShape: a cheap, tolerance-forgiving geometric
//   classifier over the raw screen-space stroke polyline (normalized
//   viewport coordinates, origin top-left). The stroke is resampled by arc
//   length so sampling rate and stroke speed never change the result.
//
//   Stage 2 — interpretStroke: a mesh-context resolver that projects the
//   EditMesh through the caller's view-projection matrix and resolves what
//   is under the stroke (empty surface / face / edge / boundary / vertex),
//   producing an INTERPRETATION RECORD: ranked candidate actions with
//   confidences and the concrete mesh elements each one would touch. The
//   record powers the interpretation chip, the debug HUD and the one-tap
//   alternative flow; it never mutates the mesh.
//
// Deterministic and header-only, like the rest of the retopo toolset. The
// older recognizeStroke() (stroke_recognizer.hpp, 3D surface polylines,
// single action) stays untouched; this is the richer screen-space path the
// shell feeds directly from Pencil samples.
namespace cyber::retopo {

// One stroke sample in normalized viewport coordinates (0..1 each axis,
// origin top-left) with its time in seconds since the stroke began.
struct ScreenSample {
    Vec2 position;
    float time = 0.0f;
};

// Stage-1 shape classes.
enum class StrokeShape {
    Unknown,     // matched nothing with useful confidence
    HoldPoint,   // stationary press (tap/hold)
    Line,        // open, straight
    ClosedLoop,  // closed polygon with 3+ corners (quad-draw shape)
    Circle,      // closed, round, no corners
    Scribble,    // open, many reversals / self-crossings
    Cross,       // open, exactly one self-crossing (an X drawn in one stroke)
    Lasso,       // closed, irregular (no corner structure, not round)
    Grid,        // open square wave ("up-across-down…", the one-stroke grid)
};

struct ShapeParams {
    int resampleCount = 64;         // arc-length resampling density
    float aspect = 1.0f;            // viewport width/height; x is multiplied
                                    // by it so angles/circles are measured in
                                    // square units, not stretched ones
    float holdMaxRadius = 0.015f;   // bounding radius below this => HoldPoint
    float holdMinDuration = 0.30f;  // seconds; full-confidence hold
    float closedFraction = 0.22f;   // endpoint gap < f * pathLength => closed
    float cornerRadians = 0.90f;    // per-window turn sharper than this is a corner
    float straightRatio = 0.90f;    // endpointDist/pathLength above this => straight
    float circleMaxDeviation = 0.18f;  // radius stddev / mean below this => round
    int scribbleMinCorners = 4;
    int scribbleMinIntersections = 2;
    int gridMinCorners = 3;         // square-wave corners for a grid stroke
    float gridPerpMaxDot = 0.55f;   // consecutive grid segments: |cos| below
                                    // this counts as perpendicular
    float gridRailMinDot = 0.80f;   // grid rails: |cos| above this counts as
                                    // mutually parallel
};

// Geometric features of the resampled stroke (kept in the record so the
// debug HUD can show WHY a stroke classified the way it did).
struct ShapeFeatures {
    float pathLength = 0.0f;
    float endpointDistance = 0.0f;
    float boundingRadius = 0.0f;  // max distance from centroid
    float duration = 0.0f;        // seconds, last sample time - first
    float straightness = 0.0f;    // endpointDistance / pathLength
    float radiusDeviation = 0.0f;  // stddev of centroid distance / mean
    int corners = 0;
    int selfIntersections = 0;
    bool closed = false;
};

struct ShapeResult {
    StrokeShape shape = StrokeShape::Unknown;
    float confidence = 0.0f;  // 0..1 heuristic score
    ShapeFeatures features;
    // Aspect-corrected, arc-length-resampled polyline stage 2 reuses.
    std::vector<Vec2> resampled;
    Vec2 centroid{};  // of the resampled polyline (aspect-corrected space)
    // For CLOSED strokes: 4 estimated corner points in normalized viewport
    // coordinates (un-aspect-corrected), ordered as a simple ring — what
    // the shell unprojects onto the Target when applying CreateQuad
    // (task 3.3). For GRID strokes: the estimated quad lattice, row-major
    // (gridRows + 1 rows of gridCols + 1 points each) in the same
    // coordinates — what the shell unprojects when applying CreateGrid
    // (task 3.4). Empty for other open shapes.
    std::vector<Vec2> corners;
    // Grid strokes only: quad-cell counts of the lattice in `corners`
    // (0 x 0 for every other shape).
    int gridRows = 0;
    int gridCols = 0;
};

// What the mesh-context resolver found under the stroke.
enum class UnderStroke {
    EmptySurface,  // no EditMesh element near the probe point
    Face,
    Edge,
    BoundaryEdge,
    Vertex,
};

// Candidate actions the grammar can currently express. Interpretation only
// — applying an action is the tool layer's job (tasks 3.3/3.4).
enum class InterpretedAction {
    None,
    CreateQuad,
    InsertLoop,
    TagLoop,
    DissolveEdge,
    DeleteFaces,
    MergeVertices,
    RotateEdge,
    TweakVertex,
    HideRegion,
    ToggleVisibility,
    CreateGrid,
};

struct ElementRef {
    enum class Kind { Vertex, Edge, Face };
    Kind kind = Kind::Vertex;
    Index id = kInvalidIndex;
    friend bool operator==(const ElementRef& a, const ElementRef& b) {
        return a.kind == b.kind && a.id == b.id;
    }
};

struct InterpretationCandidate {
    InterpretedAction action = InterpretedAction::None;
    float confidence = 0.0f;
    std::vector<ElementRef> elements;  // concrete targets, deterministic order
};

// The interpretation record (design D5): chosen interpretation first,
// viable alternatives after it, confidences and referenced mesh elements.
struct StrokeInterpretation {
    ShapeResult shape;
    UnderStroke context = UnderStroke::EmptySurface;
    std::vector<InterpretationCandidate> candidates;  // ranked, best first
};

struct ContextParams {
    float vertexRadius = 0.035f;  // screen-space pick radius for vertices
    float edgeRadius = 0.025f;    // screen-space pick radius for edges
};

namespace interp_detail {

inline float length2(Vec2 v) { return v.x * v.x + v.y * v.y; }
inline float len(Vec2 v) { return std::sqrt(length2(v)); }
inline float dot2(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float cross2(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

inline Vec2 closestOnSegment2(Vec2 a, Vec2 b, Vec2 p) {
    const Vec2 ab = b - a;
    const float l2 = length2(ab);
    if (l2 <= 0.0f) {
        return a;
    }
    const float t = std::clamp(dot2(p - a, ab) / l2, 0.0f, 1.0f);
    return a + ab * t;
}

inline bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    // Strict crossing with an epsilon dead-band: nearly-collinear segments
    // (ubiquitous after resampling a straight stroke) must not register as
    // intersections through float rounding noise.
    constexpr float kEps = 1e-7f;
    const float d1 = cross2(b - a, c - a);
    const float d2 = cross2(b - a, d - a);
    const float d3 = cross2(d - c, a - c);
    const float d4 = cross2(d - c, b - c);
    const bool split1 = (d1 > kEps && d2 < -kEps) || (d1 < -kEps && d2 > kEps);
    const bool split2 = (d3 > kEps && d4 < -kEps) || (d3 < -kEps && d4 > kEps);
    return split1 && split2;
}

// Arc-length resampling to `count` points (aspect-corrected input).
inline std::vector<Vec2> resample(std::span<const Vec2> pts, int count) {
    std::vector<Vec2> out;
    if (pts.empty() || count < 2) {
        return out;
    }
    float total = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        total += len(pts[i] - pts[i - 1]);
    }
    out.reserve(static_cast<std::size_t>(count));
    if (total <= 0.0f) {
        out.assign(static_cast<std::size_t>(count), pts.front());
        return out;
    }
    const float step = total / static_cast<float>(count - 1);
    out.push_back(pts.front());
    float carried = 0.0f;
    std::size_t seg = 1;
    Vec2 prev = pts.front();
    while (static_cast<int>(out.size()) < count - 1 && seg < pts.size()) {
        const float segLen = len(pts[seg] - prev);
        if (carried + segLen >= step && segLen > 0.0f) {
            const float t = (step - carried) / segLen;
            prev = lerp(prev, pts[seg], t);
            out.push_back(prev);
            carried = 0.0f;
        } else {
            carried += segLen;
            prev = pts[seg];
            ++seg;
        }
    }
    while (static_cast<int>(out.size()) < count) {
        out.push_back(pts.back());
    }
    return out;
}

// Number of self-crossings, ignoring the seam between first/last segments.
inline int countSelfIntersections(const std::vector<Vec2>& p) {
    int hits = 0;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        for (std::size_t j = i + 2; j + 1 < n; ++j) {
            if (i == 0 && j + 2 == n) {
                continue;
            }
            if (segmentsCross(p[i], p[i + 1], p[j], p[j + 1])) {
                ++hits;
            }
        }
    }
    return hits;
}

// Corner detection over a sliding direction window: direction is measured
// between samples `window` apart so a rounded (sloppy) corner still turns
// sharply within one window; after a detected corner the scan skips a
// window so one corner never double-counts. Returns the corner sample
// indices in stroke order.
inline std::vector<int> cornerIndices(const std::vector<Vec2>& p, int window,
                                      float cornerRadians) {
    std::vector<int> indices;
    const int n = static_cast<int>(p.size());
    if (n < 2 * window + 1) {
        return indices;
    }
    int i = window;
    while (i < n - window) {
        const Vec2 in = p[static_cast<std::size_t>(i)] - p[static_cast<std::size_t>(i - window)];
        const Vec2 out = p[static_cast<std::size_t>(i + window)] - p[static_cast<std::size_t>(i)];
        const float li = len(in);
        const float lo = len(out);
        if (li > 0.0f && lo > 0.0f) {
            const float cosA = std::clamp(dot2(in, out) / (li * lo), -1.0f, 1.0f);
            if (std::acos(cosA) > cornerRadians) {
                indices.push_back(i);
                i += window;  // suppression: one corner per window
                continue;
            }
        }
        ++i;
    }
    return indices;
}

inline int countCorners(const std::vector<Vec2>& p, int window, float cornerRadians) {
    return static_cast<int>(cornerIndices(p, window, cornerRadians).size());
}

// Estimated quad corners of a CLOSED resampled stroke (aspect-corrected
// space). When the detected stroke corners plus the seam corner (the
// start/end point, which the non-wrapping scan never registers) form
// exactly four, those are the quad, already in stroke (ring) order.
// Otherwise — circles, sloppy loops — the fallback is a stable inscribed
// quad: the extreme points along the two diagonal directions, ordered by
// angle around the centroid so the ring never self-intersects. Always
// deterministic.
inline std::vector<Vec2> quadCorners(const std::vector<Vec2>& p, Vec2 centroid, int window,
                                     float cornerRadians) {
    std::vector<Vec2> corners;
    if (p.size() < 4) {
        return corners;
    }
    const std::vector<int> detected = cornerIndices(p, window, cornerRadians);
    if (detected.size() == 4) {
        // Stroke started mid-edge: all four corners registered.
        for (const int i : detected) {
            corners.push_back(p[static_cast<std::size_t>(i)]);
        }
        return corners;
    }
    if (detected.size() == 3) {
        corners.push_back(p.front());  // seam corner
        for (const int i : detected) {
            corners.push_back(p[static_cast<std::size_t>(i)]);
        }
        return corners;
    }
    // Fallback: argmax of the dot product with the four diagonal
    // directions (ties keep the earlier sample — deterministic). A single
    // sharp extreme sample can maximize TWO diagonals at once (e.g. the
    // tip of a rounded-right-triangle / teardrop loop), and on a stroke
    // that ends exactly where it began the first and last resampled
    // samples COINCIDE — either way a repeated point would hand the caller
    // a degenerate ring create-face rightly rejects. Samples (spatially)
    // coinciding with an already-picked corner are therefore excluded;
    // when no distinct sample remains the stroke has no usable quad and
    // no corners are reported at all.
    const Vec2 dirs[4] = {{1, 1}, {1, -1}, {-1, -1}, {-1, 1}};
    constexpr float kDistinctSq = 1e-10f;  // ~1e-5 in normalized viewport
    for (const Vec2 d : dirs) {
        std::size_t best = p.size();
        float bestDot = -std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < p.size(); ++i) {
            bool duplicate = false;
            for (const Vec2 c : corners) {
                const Vec2 diff = p[i] - c;
                if (dot2(diff, diff) < kDistinctSq) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            const float dp = dot2(p[i] - centroid, d);
            if (dp > bestDot) {
                bestDot = dp;
                best = i;
            }
        }
        if (best == p.size()) {
            corners.clear();  // fewer than 4 distinct samples: no quad
            return corners;
        }
        corners.push_back(p[best]);
    }
    std::stable_sort(corners.begin(), corners.end(), [centroid](Vec2 a, Vec2 b) {
        return std::atan2(a.y - centroid.y, a.x - centroid.x) <
               std::atan2(b.y - centroid.y, b.x - centroid.x);
    });
    return corners;
}

// One-stroke grid detection ("up-across-down…", task 3.4): an open square
// wave whose corner-delimited segments are pairwise perpendicular, with the
// longer alternating family (the rails) mutually parallel. The lattice is
// the rail endpoints: row 0 at the rails' common start side, row 1 at their
// end side, one column per rail — one row of gridCols quad cells.
struct GridDetection {
    bool valid = false;
    std::vector<Vec2> lattice;  // row-major, 2 rows x (cols + 1) points
    int rows = 0;
    int cols = 0;
};

inline GridDetection detectGrid(const std::vector<Vec2>& p, int window,
                                const ShapeParams& params) {
    GridDetection out;
    const std::vector<int> cornerIdx = cornerIndices(p, window, params.cornerRadians);
    if (static_cast<int>(cornerIdx.size()) < params.gridMinCorners) {
        return out;
    }
    // Corner-delimited segments (endpoints included as waypoints).
    struct Segment {
        Vec2 a, b, dir;
        float length;
    };
    std::vector<int> waypoints;
    waypoints.reserve(cornerIdx.size() + 2);
    waypoints.push_back(0);
    waypoints.insert(waypoints.end(), cornerIdx.begin(), cornerIdx.end());
    waypoints.push_back(static_cast<int>(p.size()) - 1);
    std::vector<Segment> segments;
    segments.reserve(waypoints.size() - 1);
    for (std::size_t i = 0; i + 1 < waypoints.size(); ++i) {
        const Vec2 a = p[static_cast<std::size_t>(waypoints[i])];
        const Vec2 b = p[static_cast<std::size_t>(waypoints[i + 1])];
        const float l = len(b - a);
        if (l <= 1e-4f) {
            return out;
        }
        segments.push_back({a, b, (b - a) * (1.0f / l), l});
    }
    if (segments.size() < 3) {
        return out;
    }
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        if (std::fabs(dot2(segments[i].dir, segments[i + 1].dir)) >
            params.gridPerpMaxDot) {
            return out;  // not a square wave
        }
    }
    // Rails = the alternating family with the greater total length.
    float evenLength = 0.0f;
    float oddLength = 0.0f;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        (i % 2 == 0 ? evenLength : oddLength) += segments[i].length;
    }
    const std::size_t railParity = evenLength >= oddLength ? 0 : 1;
    std::vector<Segment> rails;
    for (std::size_t i = railParity; i < segments.size(); i += 2) {
        rails.push_back(segments[i]);
    }
    if (rails.size() < 2) {
        return out;
    }
    // Serpentine, not staircase or zig-zag: consecutive rails must run in
    // ALTERNATING directions ("up-across-DOWN-across-up"). A dissolve
    // scribble's diagonal strokes all point the same way and fail here.
    for (std::size_t i = 0; i + 1 < rails.size(); ++i) {
        if (dot2(rails[i].dir, rails[i + 1].dir) > -params.gridRailMinDot) {
            return out;
        }
    }
    const Vec2 u = rails.front().dir;
    // Orient every rail along +u; row 0 = the -u ends, row 1 = the +u ends.
    std::vector<Vec2> row0;
    std::vector<Vec2> row1;
    row0.reserve(rails.size());
    row1.reserve(rails.size());
    for (Segment rail : rails) {
        if (dot2(rail.b - rail.a, u) < 0.0f) {
            std::swap(rail.a, rail.b);
        }
        row0.push_back(rail.a);
        row1.push_back(rail.b);
    }
    out.lattice.reserve(row0.size() * 2);
    out.lattice.insert(out.lattice.end(), row0.begin(), row0.end());
    out.lattice.insert(out.lattice.end(), row1.begin(), row1.end());
    out.rows = 1;
    out.cols = static_cast<int>(rails.size()) - 1;
    out.valid = true;
    return out;
}

inline bool pointInPolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool inside = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 a = poly[i];
        const Vec2 b = poly[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const float x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

}  // namespace interp_detail

// ---- stage 1: geometric shape classification ------------------------------

[[nodiscard]] inline ShapeResult classifyShape(std::span<const ScreenSample> samples,
                                               const ShapeParams& params = {}) {
    using namespace interp_detail;
    ShapeResult result;
    if (samples.empty()) {
        return result;
    }

    // Aspect-correct so a screen-space circle/square stays one after
    // viewport normalization.
    std::vector<Vec2> raw;
    raw.reserve(samples.size());
    for (const ScreenSample& s : samples) {
        raw.push_back({s.position.x * params.aspect, s.position.y});
    }
    result.features.duration = samples.back().time - samples.front().time;

    float pathLength = 0.0f;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        pathLength += len(raw[i] - raw[i - 1]);
    }
    result.features.pathLength = pathLength;
    result.features.endpointDistance = len(raw.back() - raw.front());

    Vec2 centroid{};
    for (const Vec2 p : raw) {
        centroid = centroid + p;
    }
    centroid = centroid * (1.0f / static_cast<float>(raw.size()));
    result.centroid = centroid;
    float boundingRadius = 0.0f;
    for (const Vec2 p : raw) {
        boundingRadius = std::fmax(boundingRadius, len(p - centroid));
    }
    result.features.boundingRadius = boundingRadius;

    // Hold: the pen never left a small disc. Confidence grows with hold
    // duration (a quick tap is still most plausibly a point gesture).
    if (boundingRadius <= params.holdMaxRadius || raw.size() < 3) {
        result.shape = StrokeShape::HoldPoint;
        result.confidence =
            result.features.duration >= params.holdMinDuration ? 0.9f : 0.6f;
        result.resampled = {centroid};
        return result;
    }

    const std::vector<Vec2> pts = resample(raw, params.resampleCount);
    result.resampled = pts;

    // Radius statistics around the centroid of the *resampled* polyline
    // (uniform arc-length weighting).
    Vec2 rc{};
    for (const Vec2 p : pts) {
        rc = rc + p;
    }
    rc = rc * (1.0f / static_cast<float>(pts.size()));
    float meanR = 0.0f;
    for (const Vec2 p : pts) {
        meanR += len(p - rc);
    }
    meanR /= static_cast<float>(pts.size());
    float varR = 0.0f;
    for (const Vec2 p : pts) {
        const float d = len(p - rc) - meanR;
        varR += d * d;
    }
    varR /= static_cast<float>(pts.size());
    const float radiusDeviation = meanR > 0.0f ? std::sqrt(varR) / meanR : 0.0f;
    result.features.radiusDeviation = radiusDeviation;

    const int window = std::max(2, params.resampleCount / 16);
    result.features.corners = countCorners(pts, window, params.cornerRadians);
    result.features.selfIntersections = countSelfIntersections(pts);
    result.features.straightness =
        pathLength > 0.0f ? result.features.endpointDistance / pathLength : 0.0f;
    result.features.closed =
        result.features.endpointDistance < params.closedFraction * pathLength;

    const ShapeFeatures& f = result.features;
    if (f.closed) {
        // Corner estimates for the shell's CreateQuad application (task
        // 3.3), reported in un-aspect-corrected normalized viewport
        // coordinates so they unproject with the stroke's own space.
        result.corners = quadCorners(pts, rc, window, params.cornerRadians);
        const float invAspect = params.aspect > 0.0f ? 1.0f / params.aspect : 1.0f;
        for (Vec2& c : result.corners) {
            c.x *= invAspect;
        }
        if (f.radiusDeviation <= params.circleMaxDeviation && f.corners <= 2 &&
            f.selfIntersections == 0) {
            result.shape = StrokeShape::Circle;
            result.confidence =
                0.9f - 0.5f * (f.radiusDeviation / params.circleMaxDeviation) * 0.2f;
        } else if (f.corners >= 3 && f.corners <= 6 && f.selfIntersections == 0) {
            result.shape = StrokeShape::ClosedLoop;
            // 3-4 detected corners (the seam corner never registers) reads
            // as a quad; 5-6 is sloppier but still a closed polygon.
            result.confidence = f.corners <= 4 ? 0.85f : 0.7f;
        } else {
            result.shape = StrokeShape::Lasso;
            result.confidence = 0.6f;
        }
        return result;
    }

    // One-stroke grid before scribble: both have many corners, but only the
    // grid's segments form a clean square wave (task 3.4).
    if (f.selfIntersections == 0 && f.corners >= params.gridMinCorners) {
        const GridDetection grid = detectGrid(pts, window, params);
        if (grid.valid) {
            result.shape = StrokeShape::Grid;
            result.confidence = 0.85f;
            result.corners = grid.lattice;
            const float invAspect = params.aspect > 0.0f ? 1.0f / params.aspect : 1.0f;
            for (Vec2& c : result.corners) {
                c.x *= invAspect;
            }
            result.gridRows = grid.rows;
            result.gridCols = grid.cols;
            return result;
        }
    }

    if (f.selfIntersections == 1 && f.corners >= 1 && f.corners <= 3 &&
        f.straightness < params.straightRatio) {
        result.shape = StrokeShape::Cross;
        result.confidence = 0.8f;
        return result;
    }
    if (f.selfIntersections >= params.scribbleMinIntersections ||
        f.corners >= params.scribbleMinCorners) {
        result.shape = StrokeShape::Scribble;
        result.confidence = 0.8f;
        return result;
    }
    if (f.straightness >= params.straightRatio && f.corners <= 1) {
        result.shape = StrokeShape::Line;
        result.confidence = f.straightness;
        return result;
    }
    result.shape = StrokeShape::Unknown;
    result.confidence = 0.3f;
    return result;
}

// ---- stage 2: mesh-context resolution --------------------------------------

namespace interp_detail {

// The EditMesh projected to the same aspect-corrected normalized screen
// space as the stroke. Projection is a pure function of the caller's
// column-major view-projection matrix; vertices behind the eye (w <= 0)
// are dropped from picking.
class ScreenProjection {
public:
    ScreenProjection(const Mesh& mesh, const float m[16], float aspect) : m_mesh(mesh) {
        m_screen.resize(mesh.vertexCapacity());
        m_valid.assign(mesh.vertexCapacity(), false);
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v)) {
                continue;
            }
            const Vec3 p = mesh.position(v);
            const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
            const float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            if (cw <= 0.0f) {
                continue;
            }
            // NDC -> normalized viewport (origin top-left), then the same
            // aspect correction applied to the stroke.
            m_screen[i] = {(cx / cw * 0.5f + 0.5f) * aspect,
                           1.0f - (cy / cw * 0.5f + 0.5f)};
            m_valid[i] = true;
        }
    }

    [[nodiscard]] const Mesh& mesh() const { return m_mesh; }

    [[nodiscard]] bool valid(VertexId v) const { return m_valid[v.value]; }
    [[nodiscard]] Vec2 screen(VertexId v) const { return m_screen[v.value]; }

    [[nodiscard]] std::optional<VertexId> nearestVertex(Vec2 p, float radius) const {
        std::optional<VertexId> best;
        float bestD2 = radius * radius;
        for (Index i = 0; i < m_mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!m_mesh.isAlive(v) || !m_valid[i]) {
                continue;
            }
            const float d2 = length2(m_screen[i] - p);
            if (d2 < bestD2) {
                bestD2 = d2;
                best = v;
            }
        }
        return best;
    }

    [[nodiscard]] std::optional<EdgeId> nearestEdge(Vec2 p, float radius) const {
        std::optional<EdgeId> best;
        float bestD2 = radius * radius;
        for (Index i = 0; i < m_mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!m_mesh.isAlive(e)) {
                continue;
            }
            const auto [v0, v1] = m_mesh.edgeVertices(e);
            if (!m_valid[v0.value] || !m_valid[v1.value]) {
                continue;
            }
            const Vec2 q = closestOnSegment2(m_screen[v0.value], m_screen[v1.value], p);
            const float d2 = length2(q - p);
            if (d2 < bestD2) {
                bestD2 = d2;
                best = e;
            }
        }
        return best;
    }

    // First face whose projected polygon contains `p` (face-id order).
    [[nodiscard]] std::optional<FaceId> faceContaining(Vec2 p) const {
        for (Index i = 0; i < m_mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!m_mesh.isAlive(f)) {
                continue;
            }
            if (facePolygon(f, m_poly) && pointInPolygon(m_poly, p)) {
                return f;
            }
        }
        return std::nullopt;
    }

    // Live edges whose projected segment crosses any stroke segment,
    // deterministic edge-id order, each edge reported once.
    [[nodiscard]] std::vector<EdgeId> edgesCrossing(const std::vector<Vec2>& stroke) const {
        std::vector<EdgeId> crossed;
        for (Index i = 0; i < m_mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!m_mesh.isAlive(e)) {
                continue;
            }
            const auto [v0, v1] = m_mesh.edgeVertices(e);
            if (!m_valid[v0.value] || !m_valid[v1.value]) {
                continue;
            }
            const Vec2 a = m_screen[v0.value];
            const Vec2 b = m_screen[v1.value];
            for (std::size_t s = 0; s + 1 < stroke.size(); ++s) {
                if (segmentsCross(a, b, stroke[s], stroke[s + 1])) {
                    crossed.push_back(e);
                    break;
                }
            }
        }
        return crossed;
    }

    // Live edges with any stroke sample within `radius`, edge-id order.
    [[nodiscard]] std::vector<EdgeId> edgesNear(const std::vector<Vec2>& stroke,
                                                float radius) const {
        std::vector<EdgeId> near;
        const float r2 = radius * radius;
        for (Index i = 0; i < m_mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!m_mesh.isAlive(e)) {
                continue;
            }
            const auto [v0, v1] = m_mesh.edgeVertices(e);
            if (!m_valid[v0.value] || !m_valid[v1.value]) {
                continue;
            }
            const Vec2 a = m_screen[v0.value];
            const Vec2 b = m_screen[v1.value];
            for (const Vec2 s : stroke) {
                if (length2(closestOnSegment2(a, b, s) - s) <= r2) {
                    near.push_back(e);
                    break;
                }
            }
        }
        return near;
    }

    // Live faces whose projected centroid lies inside the stroke polygon.
    [[nodiscard]] std::vector<FaceId> facesEnclosed(const std::vector<Vec2>& stroke) const {
        std::vector<FaceId> inside;
        for (Index i = 0; i < m_mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!m_mesh.isAlive(f)) {
                continue;
            }
            if (const std::optional<Vec2> c = faceScreenCentroid(f);
                c && pointInPolygon(stroke, *c)) {
                inside.push_back(f);
            }
        }
        return inside;
    }

    // Live faces whose projected centroid lies inside the axis-aligned box
    // [lo, hi] (the X gesture's footprint), face-id order.
    [[nodiscard]] std::vector<FaceId> facesInBox(Vec2 lo, Vec2 hi) const {
        std::vector<FaceId> inside;
        for (Index i = 0; i < m_mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!m_mesh.isAlive(f)) {
                continue;
            }
            if (const std::optional<Vec2> c = faceScreenCentroid(f);
                c && c->x >= lo.x && c->x <= hi.x && c->y >= lo.y && c->y <= hi.y) {
                inside.push_back(f);
            }
        }
        return inside;
    }

    // Fraction of stroke samples within `radius` of some edge.
    [[nodiscard]] float fractionAlongEdges(const std::vector<Vec2>& stroke,
                                           float radius) const {
        if (stroke.empty()) {
            return 0.0f;
        }
        const float r2 = radius * radius;
        std::size_t hits = 0;
        for (const Vec2 s : stroke) {
            bool near = false;
            for (Index i = 0; i < m_mesh.edgeCapacity() && !near; ++i) {
                const EdgeId e{i};
                if (!m_mesh.isAlive(e)) {
                    continue;
                }
                const auto [v0, v1] = m_mesh.edgeVertices(e);
                if (!m_valid[v0.value] || !m_valid[v1.value]) {
                    continue;
                }
                near = length2(closestOnSegment2(m_screen[v0.value],
                                                 m_screen[v1.value], s) -
                               s) <= r2;
            }
            if (near) {
                ++hits;
            }
        }
        return static_cast<float>(hits) / static_cast<float>(stroke.size());
    }

private:
    bool facePolygon(FaceId f, std::vector<Vec2>& out) const {
        out.clear();
        for (const VertexId v : m_mesh.faceVertices(f)) {
            if (!m_valid[v.value]) {
                return false;
            }
            out.push_back(m_screen[v.value]);
        }
        return out.size() >= 3;
    }

    std::optional<Vec2> faceScreenCentroid(FaceId f) const {
        Vec2 acc{};
        int n = 0;
        for (const VertexId v : m_mesh.faceVertices(f)) {
            if (!m_valid[v.value]) {
                return std::nullopt;
            }
            acc = acc + m_screen[v.value];
            ++n;
        }
        return n > 0 ? std::optional<Vec2>(acc * (1.0f / static_cast<float>(n)))
                     : std::nullopt;
    }

    const Mesh& m_mesh;
    std::vector<Vec2> m_screen;
    std::vector<bool> m_valid;
    mutable std::vector<Vec2> m_poly;
};

inline void addCandidate(StrokeInterpretation& out, InterpretedAction action,
                         float confidence, std::vector<ElementRef> elements = {}) {
    out.candidates.push_back({action, confidence, std::move(elements)});
}

inline std::vector<ElementRef> edgeRefs(const std::vector<EdgeId>& edges) {
    std::vector<ElementRef> refs;
    refs.reserve(edges.size());
    for (const EdgeId e : edges) {
        refs.push_back({ElementRef::Kind::Edge, e.value});
    }
    return refs;
}

inline std::vector<ElementRef> faceRefs(const std::vector<FaceId>& faces) {
    std::vector<ElementRef> refs;
    refs.reserve(faces.size());
    for (const FaceId f : faces) {
        refs.push_back({ElementRef::Kind::Face, f.value});
    }
    return refs;
}

}  // namespace interp_detail

// Resolves the stroke against the EditMesh. `mesh`/`viewProj` may be null:
// stage 1 still runs and every context-dependent rule sees an empty scene
// (the caller has no EditMesh yet — e.g. the very first quad of a retopo).
// `viewProj` is column-major (matches simd_float4x4 memory order).
[[nodiscard]] inline StrokeInterpretation interpretStroke(
    std::span<const ScreenSample> samples, const Mesh* mesh, const float* viewProj,
    const ShapeParams& shapeParams = {}, const ContextParams& contextParams = {}) {
    using namespace interp_detail;

    StrokeInterpretation out;
    out.shape = classifyShape(samples, shapeParams);
    if (out.shape.resampled.empty()) {
        addCandidate(out, InterpretedAction::None, 0.1f);
        return out;
    }

    std::optional<ScreenProjection> projStorage;
    const ScreenProjection* proj = nullptr;
    if (mesh != nullptr && viewProj != nullptr) {
        projStorage.emplace(*mesh, viewProj, shapeParams.aspect);
        proj = &*projStorage;
    }

    const std::vector<Vec2>& stroke = out.shape.resampled;
    const Vec2 probe = out.shape.shape == StrokeShape::Line ||
                               out.shape.shape == StrokeShape::Scribble
                           ? stroke[stroke.size() / 2]
                           : out.shape.centroid;

    // Resolve the under-stroke context at the probe point.
    std::optional<VertexId> probeVertex;
    std::optional<EdgeId> probeEdge;
    std::optional<FaceId> probeFace;
    if (proj != nullptr) {
        probeVertex = proj->nearestVertex(probe, contextParams.vertexRadius);
        probeEdge = proj->nearestEdge(probe, contextParams.edgeRadius);
        probeFace = proj->faceContaining(probe);
    }
    if (probeVertex) {
        out.context = UnderStroke::Vertex;
    } else if (probeEdge) {
        out.context = proj->mesh().isBoundaryEdge(*probeEdge)
                          ? UnderStroke::BoundaryEdge
                          : UnderStroke::Edge;
    } else if (probeFace) {
        out.context = UnderStroke::Face;
    } else {
        out.context = UnderStroke::EmptySurface;
    }

    const float shapeConf = out.shape.confidence;
    switch (out.shape.shape) {
        case StrokeShape::HoldPoint: {
            if (probeVertex) {
                addCandidate(out, InterpretedAction::TweakVertex, 0.9f * shapeConf,
                             {{ElementRef::Kind::Vertex, probeVertex->value}});
            }
            break;
        }
        case StrokeShape::Line: {
            if (proj != nullptr) {
                const std::optional<VertexId> v0 =
                    proj->nearestVertex(stroke.front(), contextParams.vertexRadius);
                const std::optional<VertexId> v1 =
                    proj->nearestVertex(stroke.back(), contextParams.vertexRadius);
                if (v0 && v1 && !(*v0 == *v1)) {
                    addCandidate(out, InterpretedAction::MergeVertices, 0.9f * shapeConf,
                                 {{ElementRef::Kind::Vertex, v0->value},
                                  {ElementRef::Kind::Vertex, v1->value}});
                }
                // Loop insert vs loop tag disambiguation (spec scenario): a
                // stroke running ALONG the loop tags it — even when it
                // grazes transverse edges near vertices — while a stroke
                // CROSSING edges inserts a full loop around the quad ring.
                // Elements carry the complete walks (loops.hpp): the whole
                // edge loop for a tag, the whole ring for an insert (the
                // first ring edge seeds cyber_retopo_insert_loop).
                const std::vector<EdgeId> crossed = proj->edgesCrossing(stroke);
                const float along =
                    proj->fractionAlongEdges(stroke, contextParams.edgeRadius);
                const std::vector<EdgeId> loop =
                    probeEdge ? edgeLoopFrom(proj->mesh(), *probeEdge)
                              : std::vector<EdgeId>{};
                if (along >= 0.6f && probeEdge) {
                    addCandidate(out, InterpretedAction::TagLoop,
                                 0.8f * along * shapeConf, edgeRefs(loop));
                    if (!crossed.empty()) {
                        addCandidate(
                            out, InterpretedAction::InsertLoop, 0.4f * shapeConf,
                            edgeRefs(quadRingFromEdge(proj->mesh(), crossed.front()).edges));
                    }
                } else if (!crossed.empty()) {
                    addCandidate(
                        out, InterpretedAction::InsertLoop, 0.75f * shapeConf,
                        edgeRefs(quadRingFromEdge(proj->mesh(), crossed.front()).edges));
                    if (probeEdge) {
                        addCandidate(out, InterpretedAction::TagLoop, 0.35f * shapeConf,
                                     edgeRefs(loop));
                    }
                }
            }
            if (out.candidates.empty() && out.context == UnderStroke::EmptySurface) {
                // A straight line IN EMPTY SPACE touching nothing is the
                // visibility gesture (invert / show-all, resolved by
                // direction in the shell). A line over mesh elements that
                // matched no rule stays None — it must not hide geometry.
                addCandidate(out, InterpretedAction::ToggleVisibility, 0.5f * shapeConf);
            }
            break;
        }
        case StrokeShape::ClosedLoop: {
            const std::vector<FaceId> enclosed =
                proj != nullptr ? proj->facesEnclosed(stroke) : std::vector<FaceId>{};
            if (out.context == UnderStroke::EmptySurface && enclosed.empty()) {
                addCandidate(out, InterpretedAction::CreateQuad, shapeConf);
                addCandidate(out, InterpretedAction::HideRegion, 0.3f * shapeConf);
            } else {
                addCandidate(out, InterpretedAction::CreateQuad, 0.5f * shapeConf);
                if (!enclosed.empty()) {
                    addCandidate(out, InterpretedAction::HideRegion, 0.4f * shapeConf,
                                 faceRefs(enclosed));
                }
            }
            break;
        }
        case StrokeShape::Circle: {
            // A large round stroke enclosing several faces is a lasso in
            // circle's clothing — rotate-edge only makes sense for a small
            // circle scoped to one edge.
            const std::vector<FaceId> enclosed =
                proj != nullptr ? proj->facesEnclosed(stroke) : std::vector<FaceId>{};
            if (enclosed.size() >= 2) {
                addCandidate(out, InterpretedAction::HideRegion, 0.7f * shapeConf,
                             faceRefs(enclosed));
                if (probeEdge) {
                    addCandidate(out, InterpretedAction::RotateEdge, 0.35f * shapeConf,
                                 {{ElementRef::Kind::Edge, probeEdge->value}});
                }
            } else if (probeEdge) {
                addCandidate(out, InterpretedAction::RotateEdge, 0.85f * shapeConf,
                             {{ElementRef::Kind::Edge, probeEdge->value}});
                addCandidate(out, InterpretedAction::CreateQuad, 0.3f * shapeConf);
            } else if (out.context == UnderStroke::EmptySurface) {
                // A round closed stroke over nothing still reads as a
                // (round-ish) quad draw.
                addCandidate(out, InterpretedAction::CreateQuad, 0.6f * shapeConf);
            } else {
                addCandidate(out, InterpretedAction::CreateQuad, 0.4f * shapeConf);
            }
            break;
        }
        case StrokeShape::Scribble: {
            if (proj != nullptr) {
                const std::vector<EdgeId> near =
                    proj->edgesNear(stroke, contextParams.edgeRadius);
                if (!near.empty()) {
                    addCandidate(out, InterpretedAction::DissolveEdge, 0.8f * shapeConf,
                                 edgeRefs(near));
                } else if (probeFace) {
                    addCandidate(out, InterpretedAction::DeleteFaces, 0.7f * shapeConf,
                                 {{ElementRef::Kind::Face, probeFace->value}});
                }
            }
            break;
        }
        case StrokeShape::Cross: {
            if (proj != nullptr) {
                // X over faces/a region: every face whose centroid lies
                // under the X's footprint (its bounding box), falling back
                // to the face under the crossing point.
                Vec2 lo = stroke.front();
                Vec2 hi = stroke.front();
                for (const Vec2 s : stroke) {
                    lo = {std::fmin(lo.x, s.x), std::fmin(lo.y, s.y)};
                    hi = {std::fmax(hi.x, s.x), std::fmax(hi.y, s.y)};
                }
                std::vector<FaceId> hit = proj->facesInBox(lo, hi);
                if (hit.empty()) {
                    if (const std::optional<FaceId> center =
                            proj->faceContaining(out.shape.centroid)) {
                        hit.push_back(*center);
                    }
                }
                if (!hit.empty()) {
                    addCandidate(out, InterpretedAction::DeleteFaces, 0.85f * shapeConf,
                                 faceRefs(hit));
                }
            }
            break;
        }
        case StrokeShape::Lasso: {
            const std::vector<FaceId> enclosed =
                proj != nullptr ? proj->facesEnclosed(stroke) : std::vector<FaceId>{};
            // The hide gesture is a closed stroke STARTING in empty space
            // and crossing the mesh (spec grammar table); a lasso started
            // on the mesh still offers hide, at lower confidence.
            bool startsEmpty = true;
            if (proj != nullptr) {
                startsEmpty =
                    !proj->nearestVertex(stroke.front(), contextParams.vertexRadius) &&
                    !proj->nearestEdge(stroke.front(), contextParams.edgeRadius) &&
                    !proj->faceContaining(stroke.front());
            }
            if (!enclosed.empty()) {
                addCandidate(out, InterpretedAction::HideRegion,
                             (startsEmpty ? 0.85f : 0.5f) * shapeConf,
                             faceRefs(enclosed));
            } else {
                addCandidate(out, InterpretedAction::HideRegion, 0.6f * shapeConf);
            }
            break;
        }
        case StrokeShape::Grid: {
            // One-stroke grid → block of quads. Strongest over empty
            // surface; still offered over existing topology, weaker.
            addCandidate(
                out, InterpretedAction::CreateGrid,
                (out.context == UnderStroke::EmptySurface ? 0.9f : 0.5f) * shapeConf);
            break;
        }
        case StrokeShape::Unknown:
            break;
    }

    if (out.candidates.empty()) {
        addCandidate(out, InterpretedAction::None, 0.2f);
    }
    // Rank best-first; equal confidences keep insertion (rule) order.
    std::stable_sort(out.candidates.begin(), out.candidates.end(),
                     [](const InterpretationCandidate& a, const InterpretationCandidate& b) {
                         return a.confidence > b.confidence;
                     });
    return out;
}

}  // namespace cyber::retopo
