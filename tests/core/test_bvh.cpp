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
