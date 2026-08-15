#include <doctest.h>

#include <random>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"

using cyber::Bvh;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;

namespace {

Mesh makeCube() {
    const std::vector<Vec3> p = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    const std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    return Mesh::fromIndexed(p, f);
}

}  // namespace

TEST_CASE("closest point lands on the surface (spec: mesh-core)") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);
    REQUIRE(bvh.triangleCount() == 12);

    // Outside, facing +z: closest point is on the z=1 face.
    const auto hit = bvh.closestPoint({0.5f, 0.5f, 2.0f});
    REQUIRE(hit.point.z == doctest::Approx(1.0f));
    REQUIRE(hit.point.x == doctest::Approx(0.5f));
    REQUIRE(hit.distanceSquared == doctest::Approx(1.0f));

    // Near a corner: snaps to the corner.
    const auto corner = bvh.closestPoint({-1.0f, -1.0f, -1.0f});
    REQUIRE(corner.point.x == doctest::Approx(0.0f));
    REQUIRE(corner.point.y == doctest::Approx(0.0f));
    REQUIRE(corner.point.z == doctest::Approx(0.0f));
}

TEST_CASE("closest point matches brute force on random queries") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-2.0f, 3.0f);
    for (int i = 0; i < 200; ++i) {
        const Vec3 q{dist(rng), dist(rng), dist(rng)};
        const auto hit = bvh.closestPoint(q);

        // Brute force over all triangles of all faces.
        float best = 1e30f;
        for (Index fi = 0; fi < cube.faceCapacity(); ++fi) {
            const cyber::FaceId f{fi};
            if (!cube.isAlive(f)) {
                continue;
            }
            const auto verts = cube.faceVertices(f);
            for (std::size_t t = 2; t < verts.size(); ++t) {
                const Vec3 p = cyber::closestPointOnTriangle(q, cube.position(verts[0]),
                                                             cube.position(verts[t - 1]),
                                                             cube.position(verts[t]));
                best = std::min(best, lengthSquared(p - q));
            }
        }
        REQUIRE(hit.distanceSquared == doctest::Approx(best).epsilon(1e-4));
    }
}

TEST_CASE("raycast hits the nearest face and respects max distance") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);

    const auto hit = bvh.raycast({0.5f, 0.5f, 5.0f}, {0, 0, -1});
    REQUIRE(hit.has_value());
    REQUIRE(hit->t == doctest::Approx(4.0f));
    REQUIRE(hit->point.z == doctest::Approx(1.0f));

    REQUIRE(!bvh.raycast({0.5f, 0.5f, 5.0f}, {0, 0, -1}, 3.0f).has_value());
    REQUIRE(!bvh.raycast({0.5f, 0.5f, 5.0f}, {0, 0, 1}).has_value());
}

TEST_CASE("raycast from inside hits the far wall") {
    const Mesh cube = makeCube();
    const Bvh bvh(cube);
    const auto hit = bvh.raycast({0.5f, 0.5f, 0.5f}, {1, 0, 0});
    REQUIRE(hit.has_value());
    REQUIRE(hit->t == doctest::Approx(0.5f));
    REQUIRE(hit->point.x == doctest::Approx(1.0f));
}

TEST_CASE("empty mesh yields empty BVH") {
    const Mesh empty;
    const Bvh bvh(empty);
    REQUIRE(bvh.empty());
    REQUIRE(!bvh.raycast({0, 0, 0}, {1, 0, 0}).has_value());
}

TEST_CASE("no ray through a shared edge is rejected by both adjacent triangles") {
    // Regression for the watertight ray/triangle test in src/core/src/bvh.cpp.
    // Strict Moller-Trumbore u/v rejections let rounding make BOTH triangles
    // sharing an edge miss the same ray, so a ray that geometrically crosses a
    // closed surface reported a miss — isolated wrong texels wherever a bake
    // cage ray crosses a shared edge (src/bake/src/bake.cpp calls this copy
    // directly). The inside test now evaluates each edge in a canonical vertex
    // order, so the two triangles see bitwise opposite values.
    //
    // The same bundle runs against the accel backends' copy of the test in
    // tests/accel/test_gpu_geometry.cpp; the two cases must stay in step, as the
    // rayTriangle implementations are mirrors of each other.
    //
    // Deliberately irregular coordinates: axis-aligned vertices make too many of
    // the intermediate products exact and hide the leak.
    const Vec3 a{-0.7331f, 0.1129f, 0.4517f};
    const Vec3 b{0.6217f, -0.3384f, 0.2903f};
    const Vec3 c{0.1873f, 0.8821f, -0.5119f};
    const Vec3 d{-0.2447f, -0.9013f, -0.3761f};
    // Consistent winding across the shared edge a-b.
    const std::vector<Vec3> points{a, b, c, d};
    const std::vector<std::vector<Index>> faces{{0, 1, 2}, {1, 0, 3}};
    const Mesh mesh = Mesh::fromIndexed(points, faces);
    const Bvh bvh(mesh);
    REQUIRE(bvh.triangleCount() == 2);

    constexpr std::size_t rays = 200'000;  // bounded: a second at most
    const Vec3 normal = cyber::normalized(cyber::cross(b - a, c - a));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> along(0.05f, 0.95f);
    std::uniform_real_distribution<float> height(0.5f, 2.0f);
    std::size_t leaks = 0;
    for (std::size_t i = 0; i < rays; ++i) {
        const Vec3 target = a + (b - a) * along(rng);  // exactly on the shared edge
        const Vec3 origin = target + normal * height(rng);
        if (!bvh.raycast(origin, cyber::normalized(target - origin)).has_value()) {
            ++leaks;
        }
    }
    CAPTURE(leaks);
    REQUIRE(leaks == 0);
}
