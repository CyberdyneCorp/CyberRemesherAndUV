#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/render/overlays.hpp"

using namespace cyber;
using namespace cyber::render;

namespace {

Mesh makeQuad() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

}  // namespace

TEST_CASE("wireframe emits one segment per mesh edge") {
    const Mesh mesh = makeQuad();
    const std::vector<std::pair<Vec3, Vec3>> segs = wireframeSegments(mesh);
    CHECK(segs.size() == mesh.edgeCount());
    CHECK(segs.size() == 4);  // a single quad has four edges
}

TEST_CASE("barycentric corners cover every fan triangle") {
    const Mesh mesh = makeQuad();  // one quad -> two fan triangles
    const std::vector<Vec3> bary = barycentricCorners(mesh);
    CHECK(bary.size() == 6);  // 2 triangles * 3 corners
    // Each corner is a canonical basis vector.
    for (const Vec3& b : bary) {
        CHECK((b.x + b.y + b.z) == doctest::Approx(1.0f));
    }
}

TEST_CASE("brush ring points lie on a circle in the brush plane") {
    BrushRadiusOverlay brush;
    brush.center = Vec3{2, 3, 4};
    brush.normal = Vec3{0, 1, 0};
    brush.radius = 1.5f;
    brush.segments = 32;
    const std::vector<Vec3> ring = brushRing(brush);
    CHECK(ring.size() == 32);
    for (const Vec3& p : ring) {
        const Vec3 rel = p - brush.center;
        CHECK(length(rel) == doctest::Approx(1.5f).epsilon(0.001));
        CHECK(dot(rel, normalized(brush.normal)) == doctest::Approx(0.0f).epsilon(0.001));
    }
}

TEST_CASE("symmetry plane quad corners lie on the plane") {
    SymmetryPlaneOverlay plane;
    plane.point = Vec3{0, 1, 0};
    plane.normal = Vec3{1, 0, 0};
    plane.extent = 2.0f;
    const std::array<Vec3, 4> quad = symmetryPlaneQuad(plane);
    const Vec3 n = normalized(plane.normal);
    for (const Vec3& corner : quad) {
        CHECK(dot(n, corner - plane.point) == doctest::Approx(0.0f).epsilon(0.001));
    }
}

TEST_CASE("brush ring clamps to a minimum segment count") {
    BrushRadiusOverlay brush;
    brush.segments = 1;  // below the triangle-fan minimum
    const std::vector<Vec3> ring = brushRing(brush);
    CHECK(ring.size() >= 3);
}
