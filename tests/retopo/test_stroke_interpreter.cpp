#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/stroke_interpreter.hpp"

using cyber::EdgeId;
using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;
namespace detail = cyber::retopo::interp_detail;

namespace {

// ---- fixtures --------------------------------------------------------------

// Quad cage in z = 0 with a gentle bulge, the shape a retopology stroke draws
// over. `side` vertices per row.
Mesh makeCage(int side) {
    Mesh mesh;
    std::vector<VertexId> ids;
    ids.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(side - 1) - 0.5f;
            const float v = static_cast<float>(j) / static_cast<float>(side - 1) - 0.5f;
            ids.push_back(mesh.addVertex({u, v, 0.15f * std::sin(5.0f * u) * std::cos(5.0f * v)}));
        }
    }
    for (int j = 0; j + 1 < side; ++j) {
        for (int i = 0; i + 1 < side; ++i) {
            const std::size_t row = static_cast<std::size_t>(j * side);
            const std::size_t next = static_cast<std::size_t>((j + 1) * side);
            const VertexId quad[4] = {ids[row + static_cast<std::size_t>(i)],
                                      ids[row + static_cast<std::size_t>(i) + 1],
                                      ids[next + static_cast<std::size_t>(i) + 1],
                                      ids[next + static_cast<std::size_t>(i)]};
            mesh.addFace(quad);
        }
    }
    return mesh;
}

// Column-major view-projection looking down +z at the cage, the memory order
// the C ABI hands in (simd_float4x4).
void makeViewProj(float aspect, float m[16]) {
    const float fov = 45.0f * 3.14159265f / 180.0f;
    const float f = 1.0f / std::tan(fov * 0.5f);
    const float dist = 2.4f;
    const float zn = 0.1f;
    const float zf = 10.0f;
    const float proj[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1.0f,
                            0,          0, 2.0f * zf * zn / (zn - zf), 0};
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -dist, 1};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += proj[k * 4 + row] * view[col * 4 + k];
            }
            m[col * 4 + row] = sum;
        }
    }
}

struct Rng {
    std::uint32_t state = 0x9e3779b9u;
    float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// A spread of strokes: straight lines, closed loops, circles, zig-zags and
// single points, at sizes from a few pixels to a third of the viewport, some
// over the cage and some off it.
std::vector<std::vector<Vec2>> strokeCorpus() {
    std::vector<std::vector<Vec2>> strokes;
    Rng rng;
    for (int rep = 0; rep < 40; ++rep) {
        const float cx = rng.range(-0.1f, 1.2f);
        const float cy = rng.range(-0.1f, 1.2f);
        const float r = rng.range(0.004f, 0.30f);
        const float angle = rng.range(0.0f, 6.2831853f);
        std::vector<Vec2> line;
        for (int i = 0; i < 64; ++i) {
            const float t = static_cast<float>(i) / 63.0f - 0.5f;
            line.push_back({cx + std::cos(angle) * 2.0f * r * t, cy + std::sin(angle) * 2.0f * r * t});
        }
        strokes.push_back(line);
        std::vector<Vec2> ring;
        for (int i = 0; i <= 64; ++i) {
            const float t = 6.2831853f * static_cast<float>(i) / 64.0f;
            const float rr = r * (1.0f + 0.4f * std::sin(3.0f * t));
            ring.push_back({cx + rr * std::cos(t), cy + rr * std::sin(t)});
        }
        strokes.push_back(ring);
        std::vector<Vec2> zigzag;
        for (int i = 0; i < 48; ++i) {
            const float t = static_cast<float>(i) / 47.0f;
            zigzag.push_back({cx + 2.0f * r * (t - 0.5f), cy + r * std::sin(14.0f * t)});
        }
        strokes.push_back(zigzag);
        strokes.push_back({{cx, cy}});
    }
    return strokes;
}

// ---- reference implementations --------------------------------------------
//
// The unfiltered pairwise scans the resolver used before it learned to reject
// elements by their screen box. Every accelerated query must agree with these
// element for element and in the same order: the box tests are only allowed to
// skip work, never to change an answer.

struct Reference {
    const Mesh& mesh;
    const detail::ScreenProjection& proj;

    [[nodiscard]] bool projected(EdgeId e) const {
        const auto [v0, v1] = mesh.edgeVertices(e);
        return proj.valid(v0) && proj.valid(v1);
    }

    [[nodiscard]] std::optional<VertexId> nearestVertex(Vec2 p, float radius) const {
        std::optional<VertexId> best;
        float bestD2 = radius * radius;
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v) || !proj.valid(v)) {
                continue;
            }
            const float d2 = detail::length2(proj.screen(v) - p);
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
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!mesh.isAlive(e) || !projected(e)) {
                continue;
            }
            const auto [v0, v1] = mesh.edgeVertices(e);
            const Vec2 q = detail::closestOnSegment2(proj.screen(v0), proj.screen(v1), p);
            const float d2 = detail::length2(q - p);
            if (d2 < bestD2) {
                bestD2 = d2;
                best = e;
            }
        }
        return best;
    }

    [[nodiscard]] std::vector<EdgeId> edgesCrossing(const std::vector<Vec2>& stroke) const {
        std::vector<EdgeId> crossed;
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!mesh.isAlive(e) || !projected(e)) {
                continue;
            }
            const auto [v0, v1] = mesh.edgeVertices(e);
            for (std::size_t s = 0; s + 1 < stroke.size(); ++s) {
                if (detail::segmentsCross(proj.screen(v0), proj.screen(v1), stroke[s],
                                          stroke[s + 1])) {
                    crossed.push_back(e);
                    break;
                }
            }
        }
        return crossed;
    }

    [[nodiscard]] std::vector<EdgeId> edgesNear(const std::vector<Vec2>& stroke,
                                                float radius) const {
        std::vector<EdgeId> near;
        const float r2 = radius * radius;
        for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
            const EdgeId e{i};
            if (!mesh.isAlive(e) || !projected(e)) {
                continue;
            }
            const auto [v0, v1] = mesh.edgeVertices(e);
            for (const Vec2 s : stroke) {
                if (detail::length2(detail::closestOnSegment2(proj.screen(v0), proj.screen(v1), s) -
                                    s) <= r2) {
                    near.push_back(e);
                    break;
                }
            }
        }
        return near;
    }

    [[nodiscard]] float fractionAlongEdges(const std::vector<Vec2>& stroke, float radius) const {
        if (stroke.empty()) {
            return 0.0f;
        }
        const float r2 = radius * radius;
        std::size_t hits = 0;
        for (const Vec2 s : stroke) {
            bool near = false;
            for (Index i = 0; i < mesh.edgeCapacity() && !near; ++i) {
                const EdgeId e{i};
                if (!mesh.isAlive(e) || !projected(e)) {
                    continue;
                }
                const auto [v0, v1] = mesh.edgeVertices(e);
                near = detail::length2(
                           detail::closestOnSegment2(proj.screen(v0), proj.screen(v1), s) - s) <= r2;
            }
            if (near) {
                ++hits;
            }
        }
        return static_cast<float>(hits) / static_cast<float>(stroke.size());
    }

    [[nodiscard]] std::optional<Vec2> centroid(FaceId f) const {
        Vec2 acc{};
        int n = 0;
        for (const VertexId v : mesh.faceVertices(f)) {
            if (!proj.valid(v)) {
                return std::nullopt;
            }
            acc = acc + proj.screen(v);
            ++n;
        }
        return n > 0 ? std::optional<Vec2>(acc * (1.0f / static_cast<float>(n))) : std::nullopt;
    }

    [[nodiscard]] std::vector<Vec2> polygon(FaceId f) const {
        std::vector<Vec2> poly;
        for (const VertexId v : mesh.faceVertices(f)) {
            if (!proj.valid(v)) {
                return {};
            }
            poly.push_back(proj.screen(v));
        }
        return poly.size() >= 3 ? poly : std::vector<Vec2>{};
    }

    [[nodiscard]] std::optional<FaceId> faceContaining(Vec2 p) const {
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!mesh.isAlive(f)) {
                continue;
            }
            const std::vector<Vec2> poly = polygon(f);
            if (!poly.empty() && detail::pointInPolygon(poly, p)) {
                return f;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<FaceId> facesEnclosed(const std::vector<Vec2>& stroke) const {
        std::vector<FaceId> inside;
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!mesh.isAlive(f)) {
                continue;
            }
            if (const std::optional<Vec2> c = centroid(f); c && detail::pointInPolygon(stroke, *c)) {
                inside.push_back(f);
            }
        }
        return inside;
    }

    [[nodiscard]] std::vector<FaceId> facesInBox(Vec2 lo, Vec2 hi) const {
        std::vector<FaceId> inside;
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!mesh.isAlive(f)) {
                continue;
            }
            if (const std::optional<Vec2> c = centroid(f);
                c && c->x >= lo.x && c->x <= hi.x && c->y >= lo.y && c->y <= hi.y) {
                inside.push_back(f);
            }
        }
        return inside;
    }
};

std::vector<Index> ids(const std::vector<EdgeId>& edges) {
    std::vector<Index> out;
    out.reserve(edges.size());
    for (const EdgeId e : edges) {
        out.push_back(e.value);
    }
    return out;
}

std::vector<Index> ids(const std::vector<FaceId>& faces) {
    std::vector<Index> out;
    out.reserve(faces.size());
    for (const FaceId f : faces) {
        out.push_back(f.value);
    }
    return out;
}

std::string describe(const std::vector<retopo::ScreenSample>& samples) {
    return "stroke of " + std::to_string(samples.size()) + " samples starting at (" +
           std::to_string(samples.front().position.x) + ", " +
           std::to_string(samples.front().position.y) + ")";
}

}  // namespace

// ---- the screen-box filters ------------------------------------------------

TEST_CASE("screen box rejection never hides an element the exact test accepts") {
    // The whole speed-up rests on this: a box test may only reject pairs the
    // exact predicate would have rejected too.
    Rng rng;
    for (int i = 0; i < 20000; ++i) {
        const Vec2 a{rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
        const Vec2 b{a.x + rng.range(-0.4f, 0.4f), a.y + rng.range(-0.4f, 0.4f)};
        const Vec2 c{rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
        const Vec2 d{c.x + rng.range(-0.4f, 0.4f), c.y + rng.range(-0.4f, 0.4f)};
        if (detail::segmentsCross(a, b, c, d)) {
            REQUIRE(detail::segmentBox(a, b).overlaps(detail::segmentBox(c, d)));
        }
        const Vec2 p{rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
        const float radius = rng.range(0.0f, 0.5f);
        const Vec2 q = detail::closestOnSegment2(a, b, p);
        if (detail::length2(q - p) <= radius * radius) {
            REQUIRE(detail::segmentBox(a, b).grown(radius).mayContain(p));
        }
    }
}

TEST_CASE("a box filter with a NaN coordinate falls through to the exact test") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const detail::Box2 box{{0.0f, 0.0f}, {1.0f, 1.0f}};
    CHECK(box.mayContain({nan, 0.5f}));
    CHECK(box.overlaps({{nan, nan}, {nan, nan}}));
    CHECK(detail::segmentBox({nan, 0.0f}, {1.0f, 1.0f}).overlaps(box));
}

// ---- resolver queries against the unfiltered scans -------------------------

TEST_CASE("mesh resolver queries match an unfiltered scan on every stroke") {
    const Mesh mesh = makeCage(24);
    const float aspect = 1.5f;
    float viewProj[16];
    makeViewProj(aspect, viewProj);
    const detail::ScreenProjection proj(mesh, viewProj, aspect);
    const Reference reference{mesh, proj};
    const retopo::ContextParams params;

    for (const std::vector<Vec2>& stroke : strokeCorpus()) {
        const Vec2 probe = stroke[stroke.size() / 2];
        CHECK(proj.nearestVertex(probe, params.vertexRadius) ==
              reference.nearestVertex(probe, params.vertexRadius));
        CHECK(proj.nearestEdge(probe, params.edgeRadius) ==
              reference.nearestEdge(probe, params.edgeRadius));
        CHECK(proj.faceContaining(probe) == reference.faceContaining(probe));
        CHECK(ids(proj.edgesCrossing(stroke)) == ids(reference.edgesCrossing(stroke)));
        CHECK(ids(proj.edgesNear(stroke, params.edgeRadius)) ==
              ids(reference.edgesNear(stroke, params.edgeRadius)));
        CHECK(proj.fractionAlongEdges(stroke, params.edgeRadius) ==
              reference.fractionAlongEdges(stroke, params.edgeRadius));
        CHECK(ids(proj.facesEnclosed(stroke)) == ids(reference.facesEnclosed(stroke)));
        const detail::Box2 box = detail::polylineBox(stroke);
        CHECK(ids(proj.facesInBox(box.lo, box.hi)) == ids(reference.facesInBox(box.lo, box.hi)));
    }
}

TEST_CASE("resolver queries survive a mesh with deleted and unprojectable faces") {
    Mesh mesh = makeCage(12);
    mesh.removeFace(FaceId{3});
    mesh.removeFace(FaceId{17});
    // Behind the eye: dropped from picking, so every query must ignore it.
    mesh.setPosition(VertexId{5}, {0.0f, 0.0f, 100.0f});
    const float aspect = 1.0f;
    float viewProj[16];
    makeViewProj(aspect, viewProj);
    const detail::ScreenProjection proj(mesh, viewProj, aspect);
    const Reference reference{mesh, proj};

    for (const std::vector<Vec2>& stroke : strokeCorpus()) {
        const Vec2 probe = stroke.front();
        CHECK(proj.nearestEdge(probe, 0.05f) == reference.nearestEdge(probe, 0.05f));
        CHECK(proj.faceContaining(probe) == reference.faceContaining(probe));
        CHECK(ids(proj.edgesCrossing(stroke)) == ids(reference.edgesCrossing(stroke)));
        CHECK(ids(proj.edgesNear(stroke, 0.05f)) == ids(reference.edgesNear(stroke, 0.05f)));
        CHECK(proj.fractionAlongEdges(stroke, 0.05f) == reference.fractionAlongEdges(stroke, 0.05f));
        CHECK(ids(proj.facesEnclosed(stroke)) == ids(reference.facesEnclosed(stroke)));
    }
}

TEST_CASE("resolver queries stay exact for degenerate strokes") {
    const Mesh mesh = makeCage(10);
    const float aspect = 1.25f;
    float viewProj[16];
    makeViewProj(aspect, viewProj);
    const detail::ScreenProjection proj(mesh, viewProj, aspect);
    const Reference reference{mesh, proj};

    const std::vector<std::vector<Vec2>> strokes = {
        {},
        {{0.5f, 0.5f}},
        {{0.5f, 0.5f}, {0.5f, 0.5f}},
        {{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}},
        {{-4.0f, -4.0f}, {5.0f, 5.0f}},  // sweeps the whole viewport
    };
    for (const std::vector<Vec2>& stroke : strokes) {
        CHECK(ids(proj.edgesCrossing(stroke)) == ids(reference.edgesCrossing(stroke)));
        CHECK(ids(proj.edgesNear(stroke, 0.03f)) == ids(reference.edgesNear(stroke, 0.03f)));
        CHECK(proj.fractionAlongEdges(stroke, 0.03f) == reference.fractionAlongEdges(stroke, 0.03f));
        if (!stroke.empty()) {
            CHECK(ids(proj.facesEnclosed(stroke)) == ids(reference.facesEnclosed(stroke)));
        }
    }
}

// ---- the grammar's decisions ----------------------------------------------

TEST_CASE("interpretStroke keeps its shapes, contexts and elements over a cage") {
    const Mesh mesh = makeCage(20);
    const float aspect = 1.5f;
    float viewProj[16];
    makeViewProj(aspect, viewProj);
    retopo::ShapeParams shape;
    shape.aspect = aspect;

    struct Expected {
        retopo::InterpretedAction action;
        std::size_t elements;
    };
    struct Case {
        std::string name;
        std::vector<retopo::ScreenSample> samples;
        retopo::StrokeShape expectedShape;
        std::vector<Expected> expectedCandidates;  // ranked, best first
    };

    std::vector<retopo::ScreenSample> line;
    for (int i = 0; i < 40; ++i) {
        const float t = static_cast<float>(i) / 39.0f;
        line.push_back({{0.35f + 0.30f * t, 0.5f}, 0.01f * static_cast<float>(i)});
    }
    std::vector<retopo::ScreenSample> loop;
    const float quad[5][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
    for (int e = 0; e < 4; ++e) {
        for (int i = 0; i < 12; ++i) {
            const float t = static_cast<float>(i) / 12.0f;
            loop.push_back({{0.5f + 0.09f * (quad[e][0] + (quad[e + 1][0] - quad[e][0]) * t),
                             0.5f + 0.09f * (quad[e][1] + (quad[e + 1][1] - quad[e][1]) * t)},
                            0.01f * static_cast<float>(loop.size())});
        }
    }
    loop.push_back({{0.5f - 0.09f, 0.5f - 0.09f}, 1.0f});
    std::vector<retopo::ScreenSample> circle;
    for (int i = 0; i <= 48; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / 48.0f;
        circle.push_back({{0.5f + 0.05f * std::cos(t), 0.5f + 0.05f * std::sin(t)},
                          0.01f * static_cast<float>(i)});
    }
    std::vector<retopo::ScreenSample> scribble;
    for (int i = 0; i < 60; ++i) {
        const float t = static_cast<float>(i) / 59.0f;
        scribble.push_back({{0.35f + 0.3f * t, 0.5f + 0.06f * std::sin(18.0f * t)},
                            0.01f * static_cast<float>(i)});
    }
    std::vector<retopo::ScreenSample> hold;
    for (int i = 0; i < 12; ++i) {
        hold.push_back({{0.5f, 0.5f}, 0.05f * static_cast<float>(i)});
    }

    // The ranked records this cage and these strokes produce. They are the
    // grammar's own decisions, so a change here is a change the gesture
    // corpus has to be re-validated against — not something an optimization
    // is allowed to move.
    using Action = retopo::InterpretedAction;
    const std::vector<Case> cases = {
        {"line along a loop, ending near two vertices",
         line,
         retopo::StrokeShape::Line,
         {{Action::MergeVertices, 2}, {Action::TagLoop, 19}, {Action::InsertLoop, 20}}},
        {"closed loop over the cage",
         loop,
         retopo::StrokeShape::ClosedLoop,
         {{Action::CreateQuad, 0}, {Action::HideRegion, 70}}},
        {"circle enclosing several faces",
         circle,
         retopo::StrokeShape::Circle,
         {{Action::HideRegion, 15}, {Action::RotateEdge, 1}}},
        {"scribble over edges", scribble, retopo::StrokeShape::Scribble,
         {{Action::DissolveEdge, 163}}},
        {"hold on a vertex", hold, retopo::StrokeShape::HoldPoint, {{Action::TweakVertex, 1}}},
    };

    for (const Case& c : cases) {
        CAPTURE(c.name);
        const retopo::StrokeInterpretation record =
            retopo::interpretStroke(c.samples, &mesh, viewProj, shape);
        CHECK(record.shape.shape == c.expectedShape);
        REQUIRE(record.candidates.size() == c.expectedCandidates.size());
        for (std::size_t i = 0; i < c.expectedCandidates.size(); ++i) {
            CAPTURE(i);
            CHECK(record.candidates[i].action == c.expectedCandidates[i].action);
            CHECK(record.candidates[i].elements.size() == c.expectedCandidates[i].elements);
        }
        // Ranked best first, and every referenced element still alive.
        for (std::size_t i = 1; i < record.candidates.size(); ++i) {
            CHECK(record.candidates[i - 1].confidence >= record.candidates[i].confidence);
        }
        for (const retopo::InterpretationCandidate& candidate : record.candidates) {
            for (const retopo::ElementRef& ref : candidate.elements) {
                switch (ref.kind) {
                    case retopo::ElementRef::Kind::Vertex:
                        CHECK(mesh.isAlive(VertexId{ref.id}));
                        break;
                    case retopo::ElementRef::Kind::Edge:
                        CHECK(mesh.isAlive(EdgeId{ref.id}));
                        break;
                    case retopo::ElementRef::Kind::Face:
                        CHECK(mesh.isAlive(FaceId{ref.id}));
                        break;
                }
            }
        }
    }
}

TEST_CASE("interpretStroke does not depend on how much cage sits outside the stroke") {
    // Same stroke, same neighbourhood, two cage sizes: the record must be the
    // one the stroke's own surroundings imply, never a function of how much
    // geometry the resolver had to walk past.
    const float aspect = 1.0f;
    float viewProj[16];
    makeViewProj(aspect, viewProj);
    retopo::ShapeParams shape;
    shape.aspect = aspect;

    const Mesh small = makeCage(9);
    const Mesh large = makeCage(17);  // same surface, every quad split once
    std::vector<retopo::ScreenSample> stroke;
    for (int i = 0; i < 40; ++i) {
        const float t = static_cast<float>(i) / 39.0f;
        stroke.push_back({{0.42f + 0.16f * t, 0.5f}, 0.01f * static_cast<float>(i)});
    }
    CAPTURE(describe(stroke));
    const retopo::StrokeInterpretation a = retopo::interpretStroke(stroke, &small, viewProj, shape);
    const retopo::StrokeInterpretation b = retopo::interpretStroke(stroke, &large, viewProj, shape);
    CHECK(a.shape.shape == b.shape.shape);
    CHECK(a.shape.confidence == b.shape.confidence);
    CHECK(a.candidates.front().action == b.candidates.front().action);
}
