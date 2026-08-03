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
    // Nearly-closed quad rescue (change simplify-gesture-grammar, task 3.2).
    // An OPEN stroke that matched nothing else is a hand drawing a quad and
    // stopping short of the seam, or leaving one side open against existing
    // topology (the adjacent-quad case). The open side can be a quarter of
    // the perimeter — past closedFraction — so the strict closed test rejects
    // it and it falls to Unknown. The rescue runs LAST, after every other
    // shape has claimed its strokes, so it only ever upgrades a would-be
    // Unknown and can never steal a grid/scribble/cross (the ordering that
    // broke earlier attempts). It is gated on GEOMETRY, not corner count: a
    // smoothly drawn U registers no sharp corners at all, so the qualifier is
    // that quadCorners can recover a non-degenerate ring, not that a
    // threshold number of bends were detected.
    // endpoint gap < f * pathLength. Aggressive by design (the proposal): the
    // rescue only ever sees strokes nothing else claimed, so the ceiling can
    // sit well above closedFraction. It stays below the straightRatio (0.9)
    // line gate, so an actual straight line is still a Line, not a quad.
    // Device corpus spans 0.23–0.52; 0.65 catches the loosest with margin.
    float nearlyClosedFraction = 0.65f;
    // A self-crossing stroke is an X (delete) only when it encloses little
    // area — its diagonal path bounds almost nothing. A messy quad (closed,
    // a few interior crossings from wobble or overlap) fills most of its
    // bounding box. Device corpus: quads fill 0.59-0.71, X's 0.01-0.16, so a
    // 0.35 cut separates them with wide margin.
    float crossMaxFill = 0.35f;
    // Corner-estimate collapse: a corner whose interior angle is straighter
    // than this (cosine below it, i.e. nearer 180 degrees) is redundant, so a
    // four-corner estimate collapses to a triangle. -0.94 is ~160 degrees; a
    // real quad corner, even a skewed trapezoid's, stays well above it.
    float triangleCollapseCos = -0.94f;
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
    CreateTriangle,
    InsertLoop,
    // A near-straight stroke ACROSS A GAP, from a vertex on one open rim to a
    // vertex on the rim facing it (change add-stroke-rim-bridge): the two
    // endpoint vertices are ONE corresponding pair, and the tool layer bridges
    // the corridor between the rims with quads.
    BridgeRims,
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
// `seamWindow` > 0 additionally ignores every crossing between a LEADING and
// a TRAILING segment (task 3.1): closing a hand-drawn quad routinely
// overshoots the start, and that seam crossing must not disqualify the loop
// from being a quad. An interior crossing (an X, a scribble) is unaffected.
inline int countSelfIntersections(const std::vector<Vec2>& p, int seamWindow = 0) {
    int hits = 0;
    const int n = static_cast<int>(p.size());
    for (int i = 0; i + 1 < n; ++i) {
        for (int j = i + 2; j + 1 < n; ++j) {
            if (i == 0 && j + 2 == n) {
                continue;
            }
            if (seamWindow > 0 && i < seamWindow && j >= n - 1 - seamWindow) {
                continue;
            }
            if (segmentsCross(p[static_cast<std::size_t>(i)], p[static_cast<std::size_t>(i + 1)],
                              p[static_cast<std::size_t>(j)], p[static_cast<std::size_t>(j + 1)])) {
                ++hits;
            }
        }
    }
    return hits;
}

// Fraction of the stroke's bounding box that its closed path encloses
// (|shoelace area| / bbox area). A quad's path runs around a filled region
// (high fill); an X's diagonal path encloses almost nothing (low fill). This
// separates a messy quad — closed, with a few interior crossings from wobble
// — from a real X, which self-crosses but bounds no area.
inline float polygonFillRatio(const std::vector<Vec2>& p) {
    if (p.size() < 3) {
        return 0.0f;
    }
    float twiceArea = 0.0f;
    Vec2 lo = p[0];
    Vec2 hi = p[0];
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec2 a = p[i];
        const Vec2 b = p[(i + 1) % p.size()];
        twiceArea += a.x * b.y - b.x * a.y;
        lo = {std::fmin(lo.x, a.x), std::fmin(lo.y, a.y)};
        hi = {std::fmax(hi.x, a.x), std::fmax(hi.y, a.y)};
    }
    const float bbox = (hi.x - lo.x) * (hi.y - lo.y);
    return bbox > 1e-9f ? std::fabs(twiceArea) * 0.5f / bbox : 0.0f;
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

// True when the stroke's seam — its start/end point — is itself a sharp
// corner, i.e. the hand closed the polygon AT a vertex rather than mid-edge.
// The non-wrapping corner scan never sees the seam, so this is what lets a
// three-detected-corner stroke tell a triangle closed mid-edge (3 real
// corners) from a quad closed at a corner (3 + the seam = 4).
inline bool seamIsCorner(const std::vector<Vec2>& p, int window, float cornerRadians) {
    const int n = static_cast<int>(p.size());
    if (n < 2 * window + 1) {
        return false;
    }
    const Vec2 in = p[0] - p[static_cast<std::size_t>(n - window)];
    const Vec2 out = p[static_cast<std::size_t>(window)] - p[0];
    const float li = len(in);
    const float lo = len(out);
    if (li < 1e-6f || lo < 1e-6f) {
        return false;
    }
    const float cosA = std::clamp(dot2(in, out) / (li * lo), -1.0f, 1.0f);
    return std::acos(cosA) > cornerRadians;
}

// Robust corner estimate for a closed-ish stroke — FOUR corners for a quad,
// THREE for a triangle — resilient to rotation, wobble and interior crossings.
//
// Farthest-point method (document-scanner style) locates the four corners: the
// two most distant points are one diagonal; the two points farthest from that
// diagonal, one on each side, are the other. But the corners are returned in
// STROKE ORDER (their position along the drawn polyline), NOT sorted by angle
// around the centroid. The hand draws a quad by going around its perimeter, so
// stroke order is the drawn ring and is ALWAYS simple; angle-around-centroid
// ordering is fragile for a thin quad — two corners subtend nearly the same
// angle and can swap, turning the face into a self-intersecting bowtie.
//
// A corner whose interior angle is nearly straight is redundant — the shape is
// a triangle — so it is dropped. `collapseCos` is the cosine above which
// (i.e. angle straighter than) a corner counts as redundant.
inline std::vector<Vec2> polygonCorners(const std::vector<Vec2>& p, Vec2 centroid,
                                        float collapseCos) {
    const int n = static_cast<int>(p.size());
    if (n < 4) {
        return {};
    }
    const auto farthestIndexFrom = [&](Vec2 q) {
        int best = 0;
        float bestD = -1.0f;
        for (int i = 0; i < n; ++i) {
            const float d = dot2(p[static_cast<std::size_t>(i)] - q,
                                 p[static_cast<std::size_t>(i)] - q);
            if (d > bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    };
    const int i0 = farthestIndexFrom(centroid);
    const int i2 = farthestIndexFrom(p[static_cast<std::size_t>(i0)]);
    const Vec2 a0 = p[static_cast<std::size_t>(i0)];
    const Vec2 dir = p[static_cast<std::size_t>(i2)] - a0;
    int ip = -1;
    int in = -1;
    float bestPos = 1e-6f;
    float bestNeg = -1e-6f;
    for (int i = 0; i < n; ++i) {
        const Vec2 q = p[static_cast<std::size_t>(i)];
        const float side = dir.x * (q.y - a0.y) - dir.y * (q.x - a0.x);
        if (side > bestPos) {
            bestPos = side;
            ip = i;
        }
        if (side < bestNeg) {
            bestNeg = side;
            in = i;
        }
    }
    // STROKE ORDER: sort the corner indices, so the ring follows the drawn
    // perimeter and never self-intersects.
    std::vector<int> idx = {i0, i2};
    if (ip >= 0) {
        idx.push_back(ip);
    }
    if (in >= 0) {
        idx.push_back(in);
    }
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    // Dedup near-coincident corners SCALED to the shape (the farthest-point
    // search finds a triangle's third vertex twice, a hair apart) — a fraction
    // of the bounding radius folds those into one corner, while a real quad's
    // corners, always far apart, never merge.
    float radius = 0.0f;
    for (const int i : idx) {
        radius = std::fmax(radius, len(p[static_cast<std::size_t>(i)] - centroid));
    }
    const float mergeSq = (radius * 0.2f) * (radius * 0.2f);
    std::vector<Vec2> ring;
    for (const int i : idx) {
        const Vec2 q = p[static_cast<std::size_t>(i)];
        bool dup = false;
        for (const Vec2 r : ring) {
            if (dot2(q - r, q - r) < mergeSq) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            ring.push_back(q);
        }
    }
    if (ring.size() < 3) {
        return {};
    }
    // Drop one near-straight (redundant) corner => a triangle. At most one:
    // a quad has none, a triangle exactly one.
    if (ring.size() == 4) {
        for (std::size_t i = 0; i < 4; ++i) {
            const Vec2 prev = ring[(i + 3) % 4];
            const Vec2 cur = ring[i];
            const Vec2 next = ring[(i + 1) % 4];
            const Vec2 a = prev - cur;
            const Vec2 b = next - cur;
            const float la = len(a);
            const float lb = len(b);
            if (la < 1e-6f || lb < 1e-6f) {
                continue;
            }
            if (dot2(a, b) / (la * lb) < collapseCos) {
                ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    return ring;
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

    // A crossing in the INTERIOR of the stroke (not at the seam where a
    // closed loop's ends meet) is the signature of an X. Seam-tolerant so a
    // quad closed with a slight overshoot — which crosses only at the seam —
    // reads as zero and stays a quad, not a delete.
    const int seamWindow = std::max(2, params.resampleCount / 8);
    const int interiorCrossings = countSelfIntersections(pts, seamWindow);

    // X BEFORE closed, gated on the INTERIOR crossing count AND on enclosing
    // little area. A hand-drawn X wobbles into extra corners and sometimes a
    // second crossing; what defines it is that it crosses itself while
    // bounding almost no area (its diagonals enclose nothing). A messy quad
    // also self-crosses — from wobble or overlap near a corner — but its path
    // runs around a filled region, so the fill ratio tells them apart. This
    // runs first so an X, open or closed, goes to DeleteFaces; an overshoot
    // quad (zero interior crossings) and a filled messy quad (high fill) both
    // fall through to the closed branch and stay quads.
    if (interiorCrossings >= 1 && f.straightness < params.straightRatio &&
        polygonFillRatio(pts) < params.crossMaxFill) {
        result.shape = StrokeShape::Cross;
        result.confidence = interiorCrossings == 1 ? 0.8f : 0.7f;
        return result;
    }

    if (f.closed) {
        // Corner estimates for the shell's CreateQuad application (task
        // 3.3), reported in un-aspect-corrected normalized viewport
        // coordinates so they unproject with the stroke's own space.
        result.corners = polygonCorners(pts, rc, params.triangleCollapseCos);
        const float invAspect = params.aspect > 0.0f ? 1.0f / params.aspect : 1.0f;
        for (Vec2& c : result.corners) {
            c.x *= invAspect;
        }
        // A closed stroke that bounds a recoverable ring IS a face —
        // triangle for a validated three-corner estimate, quad otherwise —
        // GATED ON GEOMETRY, not the raw corner count. A quad drawn with
        // smooth, rounded corners registers fewer than two sharp bends, so a
        // corner-count threshold dropped it to Lasso -> hideRegion (a quad
        // that hid faces). polygonCorners recovering three or four distinct
        // extremes is the real qualifier; Lasso is left only for the
        // degenerate case where no ring could be recovered at all.
        const bool isTriangle = result.corners.size() == 3;
        const bool boundsRing = result.corners.size() >= 3;
        // A closed stroke that bounds a ring is a face. Interior crossings no
        // longer demote it: a low-area self-crossing X was already diverted
        // above (fill gate), so a closed stroke reaching here that still
        // crosses itself is a MESSY QUAD — wobble or overlap near a corner —
        // and stays a quad. Circle only needs its crossings clean.
        if (!isTriangle && f.radiusDeviation <= params.circleMaxDeviation &&
            f.corners <= 2 && interiorCrossings == 0) {
            result.shape = StrokeShape::Circle;
            result.confidence =
                0.9f - 0.5f * (f.radiusDeviation / params.circleMaxDeviation) * 0.2f;
        } else if (boundsRing) {
            result.shape = StrokeShape::ClosedLoop;
            // Sharper corner structure reads as more confidently a polygon;
            // a smooth or messy closed stroke is still a face, just less
            // crisply drawn.
            result.confidence =
                interiorCrossings == 0 && f.corners >= 3 && f.corners <= 4 ? 0.85f : 0.7f;
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

    // Nearly-closed quad rescue (task 3.2): a would-be Unknown that still
    // reads as a hand-drawn quad — no self-crossings, not a straight line,
    // endpoints within a generous fraction of the perimeter, and a
    // non-degenerate quad ring recoverable from it. Placed LAST on purpose:
    // grid/scribble/cross/line have already claimed their strokes above, so
    // this only rescues leftovers and cannot steal from them. Reported as a
    // slightly-lower-confidence ClosedLoop carrying the same estimated quad
    // corners a cleanly closed loop would, so the shell's CreateQuad
    // application is identical. No corner-count gate: a smoothly drawn U
    // registers no sharp corners, and `quadCorners` recovering four distinct
    // extremes is the real qualifier that the stroke bounds an area.
    if (f.selfIntersections == 0 && f.straightness < params.straightRatio &&
        f.endpointDistance < params.nearlyClosedFraction * pathLength) {
        // Always a QUAD estimate here, never `polygonCorners`: an open
        // stroke's two ends are far apart, so the seam-corner test triangle
        // detection relies on is meaningless and would misread a smooth
        // adjacent-quad U as a three-corner triangle. Triangles are only
        // recognized on genuinely closed strokes (the branch above).
        result.corners = polygonCorners(pts, rc, params.triangleCollapseCos);
        const float invAspect = params.aspect > 0.0f ? 1.0f / params.aspect : 1.0f;
        for (Vec2& c : result.corners) {
            c.x *= invAspect;
        }
        if (result.corners.size() >= 3) {
            result.shape = StrokeShape::ClosedLoop;
            result.confidence = 0.7f;
            return result;
        }
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

// Edge-walking quad completion (change add-context-aware-create-face). The
// existing vertex adjacent to BOTH endpoints (a shared grid corner) that lies on
// the OPPOSITE side of the A-B chord from the drawn bend C — the vertex that
// closes a hand-filled quad against the existing topology, guaranteeing a
// uniform quad rather than a geometric guess. Among candidates it prefers the
// one nearest the parallelogram estimate. nullopt when the endpoints share no
// such neighbour (e.g. drawing into empty space), where the caller falls back to
// the geometric estimate.
inline std::optional<VertexId> sharedCornerOppositeBend(
    const ScreenProjection* proj, VertexId a, VertexId b, Vec2 A, Vec2 B, Vec2 C) {
    const Mesh& mesh = proj->mesh();
    // Neighbours of A: the verts before/after A in each of A's face rings.
    std::vector<VertexId> neigh;
    for (const FaceId f : mesh.vertexFaces(a)) {
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        const std::size_t n = fv.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (fv[i] == a) {
                neigh.push_back(fv[(i + n - 1) % n]);
                neigh.push_back(fv[(i + 1) % n]);
            }
        }
    }
    const float sideC = cross2(B - A, C - A);
    const Vec2 estimate = A + B - C;
    std::optional<VertexId> best;
    float bestDist = std::numeric_limits<float>::max();
    for (const VertexId d : neigh) {
        if (d == a || d == b || !proj->valid(d)) {
            continue;
        }
        if (!mesh.edgeBetween(b, d).valid()) {
            continue;  // must also be a neighbour of B (a genuine shared corner)
        }
        const Vec2 Ds = proj->screen(d);
        // Must sit on the opposite side of the chord from the bend, so the quad
        // A-C-B-D opens correctly instead of folding.
        if ((sideC > 0.0f) == (cross2(B - A, Ds - A) > 0.0f)) {
            continue;
        }
        const float dist = length2(Ds - estimate);
        if (dist < bestDist) {
            bestDist = dist;
            best = d;
        }
    }
    return best;
}

// Context-aware open-stroke face creation (change add-context-aware-create-face).
// An OPEN stroke whose two endpoints snap to existing vertices A, B is a hand
// filling a face against adjacent topology. It is read by the sharpness of its
// dominant bend C: a sharp (~90 degrees) corner traces two sides of a rectangle
// -> QUAD [A, C, B, A+B-C] (the 4th corner completes the parallelogram, welding
// to a vertex there if one exists); a gentle bend traces two sides of a triangle
// -> TRIANGLE [A, C, B]; near-straight stays an edge/loop gesture -> no face.
// Never creates over an existing face. On success it sets out.shape.corners to
// the ring (un-aspect-corrected, like the closed path) and adds the create
// candidate first. Returns whether it fired.
inline bool tryOpenStrokeCreateFace(StrokeInterpretation& out, const ScreenProjection* proj,
                                    const ShapeParams& shapeParams,
                                    const ContextParams& contextParams) {
    if (proj == nullptr) {
        return false;
    }
    // Open adjacent-fill strokes: Line / Unknown, and ALSO ClosedLoop — an L
    // whose two arms bring its endpoints within the nearly-closed fraction is
    // rescued as a ClosedLoop (and would misread as a 3-corner triangle), but if
    // its endpoints snap to two DISTINCT vertices it is really an open fill. The
    // A != B gate below excludes genuine closed loops (whose start ~ end snap to
    // the same vertex). Grids/scribbles/crosses/circles keep their shapes.
    const StrokeShape s = out.shape.shape;
    if (s != StrokeShape::Line && s != StrokeShape::Unknown &&
        s != StrokeShape::ClosedLoop) {
        return false;
    }
    const std::vector<Vec2>& stroke = out.shape.resampled;
    if (stroke.size() < 3) {
        return false;
    }
    // Both endpoints must snap to distinct existing vertices.
    const std::optional<VertexId> a =
        proj->nearestVertex(stroke.front(), contextParams.vertexRadius);
    const std::optional<VertexId> b =
        proj->nearestVertex(stroke.back(), contextParams.vertexRadius);
    if (!a || !b || *a == *b) {
        return false;
    }
    const Vec2 A = proj->screen(*a);
    const Vec2 B = proj->screen(*b);
    const Vec2 chord = B - A;
    const float chordLen2 = length2(chord);
    if (chordLen2 < 1e-8f) {
        return false;
    }
    // Snap an inferred corner to a nearby existing vertex so the face conforms
    // to the existing grid (uniform quads for animation/sculpting) rather than
    // inventing a skewed corner. More generous than the endpoint pick radius
    // because a completion is only an estimate. A and B already sit on vertices.
    const float inferRadius = contextParams.vertexRadius * 1.6f;
    const auto snapInferred = [&](Vec2 corner) -> Vec2 {
        if (const std::optional<VertexId> v = proj->nearestVertex(corner, inferRadius)) {
            return proj->screen(*v);
        }
        return corner;
    };

    std::vector<Vec2> ring;
    InterpretedAction action;
    // Count the stroke's REAL corners (windowed turn). Two corners = a U / three-
    // sided fill whose four quad corners ARE the endpoints plus the two bends, in
    // stroke order — no inference. (The single dominant-bend guess below fails on
    // a U: it picks the midpoint of the far side and folds the quad.)
    const int window = std::max(2, shapeParams.resampleCount / 16);
    const std::vector<int> corners = cornerIndices(stroke, window, shapeParams.cornerRadians);
    // ... but ONE ROUNDED BEND is not two corners (change fix-quad-rim-sharing).
    // `cornerIndices` skips one window after a corner, and a hand-drawn rounded turn
    // is still turning after that skip, so it reports the same bend twice — measured
    // on device at 4 samples and 0.08 of the chord apart. Trusted as a U's two bends
    // that builds a ring with two nearly coincident corners: a face stretched across
    // everything between the endpoints. Two bends must therefore be separated BOTH
    // along the stroke and in space; an L falls through to the single-bend read it
    // should always have had.
    const bool twoBends =
        corners.size() >= 2 &&
        corners.back() - corners.front() >= static_cast<int>(stroke.size()) / 5 &&
        length2(stroke[static_cast<std::size_t>(corners.back())] -
                stroke[static_cast<std::size_t>(corners.front())]) > 0.0625f * chordLen2;
    if (twoBends) {
        const Vec2 C1 = snapInferred(stroke[static_cast<std::size_t>(corners.front())]);
        const Vec2 C2 = snapInferred(stroke[static_cast<std::size_t>(corners.back())]);
        ring = {A, C1, C2, B};
        action = InterpretedAction::CreateQuad;
    } else {
        // Single bend (L): the stroke sample of maximum perpendicular distance
        // from the A-B chord, classified by its interior angle.
        Vec2 C = stroke[stroke.size() / 2];
        float bestPerp = -1.0f;
        for (const Vec2 p : stroke) {
            const float t = std::clamp(dot2(p - A, chord) / chordLen2, 0.0f, 1.0f);
            const float perp = len(p - (A + chord * t));
            if (perp > bestPerp) {
                bestPerp = perp;
                C = p;
            }
        }
        if (bestPerp < 0.12f * std::sqrt(chordLen2)) {
            return false;  // near-straight: an edge/loop gesture, not a face
        }
        const Vec2 ca = A - C;
        const Vec2 cb = B - C;
        const float la = len(ca);
        const float lb = len(cb);
        if (la < 1e-6f || lb < 1e-6f) {
            return false;
        }
        const float cosC = dot2(ca, cb) / (la * lb);
        if (cosC > -0.5f) {  // sharp ~60-120 degrees: two sides of a rectangle -> quad
            const Vec2 Cs = snapInferred(C);
            // Prefer the edge-walked shared corner (guaranteed on the existing
            // grid -> uniform); fall back to the snapped parallelogram estimate
            // when the endpoints share no such neighbour (filling empty space).
            Vec2 D;
            if (const std::optional<VertexId> shared =
                    sharedCornerOppositeBend(proj, *a, *b, A, B, Cs)) {
                D = proj->screen(*shared);
            } else {
                D = snapInferred(A + B - Cs);
            }
            ring = {A, Cs, B, D};
            action = InterpretedAction::CreateQuad;
        } else if (cosC > -0.94f) {  // gentle ~120-160 degrees: two sides of a triangle
            ring = {A, snapInferred(C), B};
            action = InterpretedAction::CreateTriangle;
        } else {
            return false;  // near-straight (>160 degrees): no face
        }
    }
    // Reject a self-intersecting (bow-tie / dart) ring: a snapped corner that
    // still folds the quad would build a malformed face — better to create
    // nothing than a bad quad.
    if (ring.size() == 4 &&
        (segmentsCross(ring[0], ring[1], ring[2], ring[3]) ||
         segmentsCross(ring[1], ring[2], ring[3], ring[0]))) {
        return false;
    }
    // Corners that collapsed onto the same point after snapping are degenerate.
    for (std::size_t i = 0; i < ring.size(); ++i) {
        for (std::size_t j = i + 1; j < ring.size(); ++j) {
            if (length2(ring[i] - ring[j]) < 1e-8f) {
                return false;
            }
        }
    }
    // Never create a face over an existing one.
    Vec2 centroid{0.0f, 0.0f};
    for (const Vec2 p : ring) {
        centroid = centroid + p;
    }
    centroid = centroid * (1.0f / static_cast<float>(ring.size()));
    if (proj->faceContaining(centroid)) {
        return false;
    }
    // Report corners un-aspect-corrected (matching the closed-stroke path).
    const float invAspect = shapeParams.aspect > 0.0f ? 1.0f / shapeParams.aspect : 1.0f;
    for (Vec2& c : ring) {
        c.x *= invAspect;
    }
    out.shape.corners = ring;
    addCandidate(out, action, 0.85f,
                 {{ElementRef::Kind::Vertex, a->value}, {ElementRef::Kind::Vertex, b->value}});
    return true;
}

// Does the stroke actually RUN OVER existing faces (change add-stroke-rim-bridge)?
//
// This is the discriminator between the two near-straight gestures, which are
// otherwise identical in shape: a LoopCut runs over faces, while a stroke across
// a GAP between two rims touches faces only where it starts and ends. Edge
// crossing cannot tell them apart — a stroke anchored on two rim vertices
// necessarily crosses those vertices' rim edges, which is exactly how a
// gap-crossing stroke came to insert a loop into a ring it merely clipped.
//
// The endpoint sixths are deliberately ignored: a stroke that starts ON a rim
// vertex sits at the corner of that vertex's faces, and a point-in-polygon test
// exactly at a polygon corner is a coin flip, so the endpoints must not decide
// this.
inline bool strokeCoversFace(const ScreenProjection* proj, const std::vector<Vec2>& stroke) {
    if (proj == nullptr || stroke.empty()) {
        return false;
    }
    const std::size_t skip = stroke.size() / 6;
    for (std::size_t i = skip; i + skip < stroke.size(); ++i) {
        if (proj->faceContaining(stroke[i]).has_value()) {
            return true;
        }
    }
    return false;
}

// True when `v` sits on an OPEN RIM — it carries at least one boundary edge. A
// bridge quad is always built on such an edge, which is what makes "never create
// over an existing face" a topological guarantee for the bridge rather than a
// geometric test: a boundary edge has one incident face, and the bridge gives it
// its second.
inline bool onOpenRim(const Mesh& mesh, VertexId v) {
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (mesh.isAlive(e) && mesh.isBoundaryEdge(e)) {
            return true;
        }
    }
    return false;
}

// Rim bridge (change add-stroke-rim-bridge): a stroke drawn ACROSS AN UNFILLED
// GAP — from a vertex on one open rim, over bare Target, to a vertex on the rim
// facing it — fills the corridor between the two rims with quads.
//
// The recognizer only names the CORRESPONDING PAIR (the two endpoint vertices);
// walking the rims outward from that pair and emitting the quads is the tool
// layer's job, as with every other action here. Confidence sits just under the
// create actions, so a bent stroke that also reads as a quad keeps the quad and
// offers the bridge as its one-tap alternative.
inline bool tryOpenStrokeBridgeRims(StrokeInterpretation& out, const ScreenProjection* proj,
                                    const ContextParams& contextParams) {
    if (proj == nullptr) {
        return false;
    }
    const StrokeShape s = out.shape.shape;
    if (s != StrokeShape::Line && s != StrokeShape::Unknown) {
        return false;
    }
    const std::vector<Vec2>& stroke = out.shape.resampled;
    if (stroke.size() < 3) {
        return false;
    }
    // Over faces this is the kept LoopCut gesture, not a gap crossing.
    if (strokeCoversFace(proj, stroke)) {
        return false;
    }
    // The MIDDLE of a gap crossing sits in open space. A stroke tracing a RIM
    // also passes the no-faces test — a rim lies on the boundary of its faces,
    // not inside them — but its middle runs along an edge, and bridging two
    // points of one rim stretch would throw a quad over the topology between
    // them.
    //
    // Measured against the stroke's OWN LENGTH rather than the pick radius,
    // which would make this depend on camera distance: at tablet zoom a real gap
    // crossing spans only a few percent of the viewport, so a fixed radius
    // covers most of it and would refuse the gesture this rule exists to accept.
    const Vec2 mid = stroke[stroke.size() / 2];
    const float span = len(stroke.back() - stroke.front());
    if (proj->nearestEdge(mid, span * 0.15f).has_value()) {
        return false;
    }
    const std::optional<VertexId> a =
        proj->nearestVertex(stroke.front(), contextParams.vertexRadius);
    const std::optional<VertexId> b =
        proj->nearestVertex(stroke.back(), contextParams.vertexRadius);
    if (!a || !b || *a == *b) {
        return false;
    }
    const Mesh& mesh = proj->mesh();
    // Already adjacent: there is no gap between them to bridge.
    if (mesh.edgeBetween(*a, *b).valid()) {
        return false;
    }
    if (!onOpenRim(mesh, *a) || !onOpenRim(mesh, *b)) {
        return false;
    }
    addCandidate(out, InterpretedAction::BridgeRims, 0.8f * out.shape.confidence,
                 {{ElementRef::Kind::Vertex, a->value}, {ElementRef::Kind::Vertex, b->value}});
    return true;
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

    // A closed stroke drawn ENTIRELY INSIDE one existing face, sharing none of its topology.
    //
    // Such a stroke would create a face nested inside another: geometrically contained,
    // topologically disconnected from it. That is never a valid quad cage — a face may sit beside
    // another (sharing an edge) or replace it, never float inside it — so the create actions are
    // withheld rather than merely down-weighted. Down-weighting is what produced the sliver
    // triangles and quads users were finding stranded inside their cage: a low-confidence create is
    // still a create.
    //
    // Deliberately strict about what counts as "sharing topology": if ANY sample lies within pick
    // range of an existing edge or vertex, the stroke is NOT interior, and the create proceeds. That
    // preserves the legitimate case of drawing a quad against existing geometry — the grid
    // continuation that shares a boundary — which is the whole reason this is a containment test
    // rather than a blanket "no creating over faces" rule.
    const auto strokeIsNestedInsideOneFace = [&]() -> bool {
        if (proj == nullptr) {
            return false;
        }
        // Must be over a face. Deliberately NOT also requiring `facesEnclosed` to be empty: a small
        // loop near a big face's centre CONTAINS that face's centroid, so the enclosure test reports
        // the very face the loop is nested inside — which made the first version of this check reject
        // the exact case it exists to catch. A test caught it.
        //
        // The containment below is the real invariant and needs no enclosure test: a loop that
        // genuinely encloses a face must extend beyond that face's boundary, so its samples leave the
        // face or pass near an edge, and it is not nested.
        if (!probeFace.has_value()) {
            return false;
        }
        for (const Vec2 sample : stroke) {
            if (proj->nearestVertex(sample, contextParams.vertexRadius).has_value() ||
                proj->nearestEdge(sample, contextParams.edgeRadius).has_value()) {
                return false;  // touches existing topology, so not nested
            }
            // Leaving the face means the stroke spans more than one, so it is not interior to one.
            const std::optional<FaceId> here = proj->faceContaining(sample);
            if (!here.has_value() || here->value != probeFace->value) {
                return false;
            }
        }
        return true;
    };

    // Context-aware open-stroke face creation (change add-context-aware-create-
    // face): an open stroke between two vertices makes a welded quad/triangle.
    // If it fires, its create candidate leads; the switch below still runs so a
    // Line keeps its insert-loop alternative.
    tryOpenStrokeCreateFace(out, proj, shapeParams, contextParams);
    // Rim bridge (change add-stroke-rim-bridge): the same open stroke read as a
    // gap crossing when it runs between two rim vertices over no face at all.
    // Ranked below a create, so a stroke that traces a face keeps the face.
    tryOpenStrokeBridgeRims(out, proj, contextParams);

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
            // A line CROSSING a face ring inserts a loop (the kept LoopCut
            // gesture); the whole quad ring the line crosses is the target
            // (loops.hpp), the first ring edge seeding cyber_retopo_insert_loop.
            // mergeVertices, tagLoop and toggleVisibility are tools now, not
            // stroke gestures, so a line that merely runs along an edge or
            // over empty space resolves to nothing.
            //
            // The line must RUN OVER FACES (change add-stroke-rim-bridge).
            // Crossed edges alone are not enough: a line anchored on two rim
            // vertices crosses those vertices' rim edges without ever running
            // along a ring, and on device that clipped crossing inserted a loop
            // nowhere near where the user drew. Such a stroke is a gap crossing
            // — the rim bridge above — not a loop cut.
            if (proj != nullptr && strokeCoversFace(proj, stroke)) {
                const std::vector<EdgeId> crossed = proj->edgesCrossing(stroke);
                if (!crossed.empty()) {
                    addCandidate(
                        out, InterpretedAction::InsertLoop, 0.75f * shapeConf,
                        edgeRefs(quadRingFromEdge(proj->mesh(), crossed.front()).edges));
                }
            }
            break;
        }
        case StrokeShape::ClosedLoop: {
            const std::vector<FaceId> enclosed =
                proj != nullptr ? proj->facesEnclosed(stroke) : std::vector<FaceId>{};
            // Three estimated corners = a triangle, four = a quad. The
            // estimate is conservative (polygonCorners falls back to a quad
            // on any ambiguity), so a triangle is only offered when the
            // corner structure is unmistakably three-sided.
            const InterpretedAction createAction =
                out.shape.corners.size() == 3 ? InterpretedAction::CreateTriangle
                                              : InterpretedAction::CreateQuad;
            // A closed loop always creates a face (hideRegion is no longer a
            // stroke gesture — it is a tool). Full confidence over empty
            // surface; a little lower when drawn over existing geometry,
            // where the intent is marginally less certain.
            const bool overEmpty =
                out.context == UnderStroke::EmptySurface && enclosed.empty();
            // Withheld entirely, not down-weighted: a face nested inside another is never valid.
            if (!strokeIsNestedInsideOneFace()) {
                addCandidate(out, createAction, overEmpty ? shapeConf : 0.5f * shapeConf);
            }
            break;
        }
        case StrokeShape::Circle: {
            // A round closed stroke is a (round-ish) quad draw. rotateEdge and
            // hideRegion are tools now, not stroke gestures — a circle only
            // ever creates a quad.
            if (!strokeIsNestedInsideOneFace()) {
                addCandidate(
                    out, InterpretedAction::CreateQuad,
                    (out.context == UnderStroke::EmptySurface ? 0.6f : 0.4f) * shapeConf);
            }
            break;
        }
        case StrokeShape::Scribble: {
            // Scribbling over geometry is a delete gesture (dissolveEdge is a
            // tool now, not a stroke): whatever faces the scribble covers are
            // deleted. Most scribbles self-cross and are already caught as an
            // X above; this remains for a non-crossing zig-zag.
            if (proj != nullptr) {
                Vec2 lo = stroke.front();
                Vec2 hi = stroke.front();
                for (const Vec2 s : stroke) {
                    lo = {std::fmin(lo.x, s.x), std::fmin(lo.y, s.y)};
                    hi = {std::fmax(hi.x, s.x), std::fmax(hi.y, s.y)};
                }
                std::vector<FaceId> hit = proj->facesInBox(lo, hi);
                if (hit.empty()) {
                    if (probeFace) {
                        hit.push_back(*probeFace);
                    }
                }
                if (!hit.empty()) {
                    addCandidate(out, InterpretedAction::DeleteFaces, 0.7f * shapeConf,
                                 faceRefs(hit));
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
            // Lasso is no longer a stroke gesture: hideRegion moved to the
            // tool layer, and a closed stroke that bounds a ring is now a
            // quad (see classifyShape). Only a degenerate closed stroke —
            // one from which no quad ring could be recovered — still lands
            // here, and it resolves to nothing rather than hiding faces.
            break;
        }
        case StrokeShape::Grid:
            // createGrid is retired from the stroke grammar — a one-stroke
            // grid is a tool now, not a Pencil gesture. The Grid SHAPE still
            // classifies (classifyShape is unchanged) but resolves to nothing.
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
