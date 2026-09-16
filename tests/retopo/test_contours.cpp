#include <doctest.h>

#include <cmath>
#include <span>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/contours.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
namespace retopo = cyber::retopo;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A cylinder about +Y from y0 to y1, triangulated, open at both ends.
Mesh cylinder(float radius, float y0, float y1, int around = 48, int along = 24) {
    Mesh m;
    std::vector<std::vector<VertexId>> rows;
    rows.reserve(static_cast<std::size_t>(along + 1));
    for (int j = 0; j <= along; ++j) {
        const float y = y0 + (y1 - y0) * static_cast<float>(j) / static_cast<float>(along);
        std::vector<VertexId> row;
        row.reserve(static_cast<std::size_t>(around));
        for (int i = 0; i < around; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(around);
            row.push_back(m.addVertex(Vec3{radius * std::cos(a), y, radius * std::sin(a)}));
        }
        rows.push_back(std::move(row));
    }
    for (int j = 0; j < along; ++j) {
        for (int i = 0; i < around; ++i) {
            const int i1 = (i + 1) % around;
            const auto& a = rows[static_cast<std::size_t>(j)];
            const auto& b = rows[static_cast<std::size_t>(j + 1)];
            m.addFace(std::array<VertexId, 3>{a[static_cast<std::size_t>(i)],
                                              a[static_cast<std::size_t>(i1)],
                                              b[static_cast<std::size_t>(i1)]});
            m.addFace(std::array<VertexId, 3>{a[static_cast<std::size_t>(i)],
                                              b[static_cast<std::size_t>(i1)],
                                              b[static_cast<std::size_t>(i)]});
        }
    }
    return m;
}

// An arc of stroke samples ON the cylinder at height y, spanning `sweep`
// radians — what an artist's cross-section stroke actually looks like: it
// covers the near side only.
std::vector<Vec3> arcStroke(float radius, float y, float sweep = 2.2f, int n = 14) {
    std::vector<Vec3> s;
    s.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float a = -sweep * 0.5f + sweep * static_cast<float>(i) / static_cast<float>(n - 1);
        s.push_back(Vec3{radius * std::cos(a), y, radius * std::sin(a)});
    }
    return s;
}

std::vector<std::span<const Vec3>> spans(const std::vector<std::vector<Vec3>>& strokes) {
    std::vector<std::span<const Vec3>> out;
    out.reserve(strokes.size());
    for (const auto& s : strokes) {
        out.emplace_back(s);
    }
    return out;
}

}  // namespace

TEST_CASE("contours lofts cross-section strokes into a quad tube") {
    const Mesh target = cylinder(1.0f, -1.0f, 1.0f);
    const retopo::SurfaceSnapper snap(target);

    const std::vector<std::vector<Vec3>> strokes{arcStroke(1.0f, -0.6f), arcStroke(1.0f, 0.0f),
                                                 arcStroke(1.0f, 0.6f)};
    const auto views = spans(strokes);

    Mesh edit;
    const retopo::ContourResult r = retopo::contours(edit, target, views, 12, &snap);

    CHECK(r.failedStroke == retopo::ContourResult::npos);
    CHECK(r.ringCount == 3);
    CHECK(r.vertices.size() == 36);
    // Two bands of 12 quads between three rings.
    CHECK(r.faces.size() == 24);
    CHECK(edit.validate().empty());

    for (const FaceId f : r.faces) {
        CHECK(edit.faceSize(f) == 4);
    }

    // The far side the artist never drew on is in the ring: every ring must
    // span the full circle, not just the 2.2 rad the stroke covered.
    for (const VertexId v : r.vertices) {
        const Vec3 p = edit.position(v);
        const float radial = std::sqrt(p.x * p.x + p.z * p.z);
        CHECK(radial == doctest::Approx(1.0f).epsilon(0.02f));
    }
    float minAngle = kPi, maxAngle = -kPi;
    for (std::size_t k = 0; k < 12; ++k) {
        const Vec3 p = edit.position(r.vertices[k]);
        const float a = std::atan2(p.z, p.x);
        minAngle = std::min(minAngle, a);
        maxAngle = std::max(maxAngle, a);
    }
    CHECK((maxAngle - minAngle) > 4.0f);  // far more than the stroke's 2.2 rad sweep
}

TEST_CASE("contour seam runs straight instead of spiralling") {
    const Mesh target = cylinder(1.0f, -1.0f, 1.0f);
    const retopo::SurfaceSnapper snap(target);

    // Deliberately awkward: each stroke is centred on a different angle, so a
    // naive resampling would start each ring somewhere else and the seam would
    // walk around the tube.
    std::vector<std::vector<Vec3>> strokes;
    for (int j = 0; j < 4; ++j) {
        std::vector<Vec3> s;
        const float y = -0.6f + 0.4f * static_cast<float>(j);
        const float centre = 1.3f * static_cast<float>(j);
        for (int i = 0; i < 14; ++i) {
            const float a = centre - 1.1f + 2.2f * static_cast<float>(i) / 13.0f;
            s.push_back(Vec3{std::cos(a), y, std::sin(a)});
        }
        strokes.push_back(std::move(s));
    }
    const auto views = spans(strokes);

    Mesh edit;
    const retopo::ContourResult r = retopo::contours(edit, target, views, 16, &snap);
    REQUIRE(r.failedStroke == retopo::ContourResult::npos);
    REQUIRE(r.ringCount == 4);

    // Column 0 of every ring must stay at roughly the same angle: that IS the
    // seam not spiralling, stated as a property of the output.
    const Vec3 first = edit.position(r.vertices[0]);
    const float baseAngle = std::atan2(first.z, first.x);
    for (std::size_t ring = 1; ring < r.ringCount; ++ring) {
        const Vec3 p = edit.position(r.vertices[ring * 16]);
        float delta = std::atan2(p.z, p.x) - baseAngle;
        while (delta > kPi) {
            delta -= 2.0f * kPi;
        }
        while (delta < -kPi) {
            delta += 2.0f * kPi;
        }
        // One span of a 16-span ring is 0.39 rad; stay well inside it.
        CHECK(std::abs(delta) < 0.30f);
    }
}

TEST_CASE("contour bands share one orientation") {
    const Mesh target = cylinder(1.0f, -1.0f, 1.0f);
    const retopo::SurfaceSnapper snap(target);
    const std::vector<std::vector<Vec3>> strokes{arcStroke(1.0f, -0.5f), arcStroke(1.0f, 0.0f),
                                                 arcStroke(1.0f, 0.5f)};
    const auto views = spans(strokes);

    Mesh edit;
    const retopo::ContourResult r = retopo::contours(edit, target, views, 10, &snap);
    REQUIRE(r.faces.size() == 20);

    // Every quad's geometric normal must point the same way relative to the
    // tube axis: a flipped band shows up as a sign change here.
    int outward = 0;
    int inward = 0;
    for (const FaceId f : r.faces) {
        const std::vector<VertexId> ring = edit.faceVertices(f);
        REQUIRE(ring.size() == 4);
        const Vec3 a = edit.position(ring[0]);
        const Vec3 b = edit.position(ring[1]);
        const Vec3 c = edit.position(ring[2]);
        const Vec3 n = cross(b - a, c - a);
        const Vec3 centre = (a + b + c) * (1.0f / 3.0f);
        const Vec3 radial{centre.x, 0.0f, centre.z};
        (dot(n, radial) > 0.0f ? outward : inward) += 1;
    }
    CHECK((outward == 0 || inward == 0));
}

TEST_CASE("contours refuse a stroke that names no plane") {
    const Mesh target = cylinder(1.0f, -1.0f, 1.0f);

    SUBCASE("a straight stroke fits every plane through its line") {
        std::vector<Vec3> straight;
        for (int i = 0; i < 10; ++i) {
            straight.push_back(Vec3{0.0f, -0.5f + 0.1f * static_cast<float>(i), 1.0f});
        }
        const std::vector<std::vector<Vec3>> strokes{arcStroke(1.0f, -0.5f), straight};
        const auto views = spans(strokes);
        Mesh edit;
        const retopo::ContourResult r = retopo::contours(edit, target, views, 8);
        CHECK(r.failedStroke == 1);
        CHECK(r.faces.empty());
        CHECK(edit.faceCount() == 0);
    }

    SUBCASE("a stroke collapsed to a point") {
        const std::vector<Vec3> dot(8, Vec3{1.0f, 0.0f, 0.0f});
        const std::vector<std::vector<Vec3>> strokes{dot, arcStroke(1.0f, 0.5f)};
        const auto views = spans(strokes);
        Mesh edit;
        const retopo::ContourResult r = retopo::contours(edit, target, views, 8);
        CHECK(r.failedStroke == 0);
        CHECK(r.faces.empty());
    }

    SUBCASE("a plane that misses the Target entirely") {
        std::vector<Vec3> away;
        for (int i = 0; i < 12; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / 12.0f;
            away.push_back(Vec3{50.0f + std::cos(a), 50.0f + std::sin(a), 50.0f});
        }
        const std::vector<std::vector<Vec3>> strokes{arcStroke(1.0f, 0.0f), away};
        const auto views = spans(strokes);
        Mesh edit;
        const retopo::ContourResult r = retopo::contours(edit, target, views, 8);
        CHECK(r.failedStroke == 1);
        CHECK(r.faces.empty());
    }
}

TEST_CASE("a closed contour run wraps the last ring to the first") {
    const Mesh target = cylinder(1.0f, -1.0f, 1.0f);
    const retopo::SurfaceSnapper snap(target);
    const std::vector<std::vector<Vec3>> strokes{arcStroke(1.0f, -0.5f), arcStroke(1.0f, 0.0f),
                                                 arcStroke(1.0f, 0.5f)};
    const auto views = spans(strokes);

    Mesh open;
    const retopo::ContourResult a = retopo::contours(open, target, views, 9, &snap, false);
    Mesh wrapped;
    const retopo::ContourResult b = retopo::contours(wrapped, target, views, 9, &snap, true);

    CHECK(a.faces.size() == 18);  // two bands
    CHECK(b.faces.size() == 27);  // three: the wrap band closes the loop
    CHECK(b.vertices.size() == a.vertices.size());
}

TEST_CASE("contours reject unusable span counts and stroke counts") {
    const Mesh target = cylinder(1.0f, -1.0f, 1.0f);
    const std::vector<std::vector<Vec3>> one{arcStroke(1.0f, 0.0f)};
    const std::vector<std::vector<Vec3>> two{arcStroke(1.0f, -0.4f), arcStroke(1.0f, 0.4f)};

    Mesh edit;
    CHECK(retopo::contours(edit, target, spans(one), 8).faces.empty());  // one ring is no tube
    CHECK(retopo::contours(edit, target, spans(two), 2).faces.empty());  // a 2-span ring is a line
    CHECK(edit.faceCount() == 0);
}
