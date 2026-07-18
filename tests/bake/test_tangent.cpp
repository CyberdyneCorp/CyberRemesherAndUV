#include <doctest.h>

#include <cmath>

#include "cyber/bake/tangent.hpp"

using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;

TEST_CASE("tangent frame is orthonormal and follows the UV gradient") {
    // A triangle in the z = 0 plane whose UVs equal its xy coordinates: the
    // tangent must point along +U (=+X), bitangent along +V (=+Y).
    const bake::TangentFrame f = bake::tangentFrame({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, Vec2{0, 0},
                                                    Vec2{1, 0}, Vec2{0, 1}, Vec3{0, 0, 1});

    REQUIRE(f.tangent.x == doctest::Approx(1.0f));
    REQUIRE(f.tangent.y == doctest::Approx(0.0f));
    REQUIRE(f.bitangent.y == doctest::Approx(1.0f));
    REQUIRE(f.normal.z == doctest::Approx(1.0f));

    // Orthonormal basis.
    REQUIRE(length(f.tangent) == doctest::Approx(1.0f));
    REQUIRE(length(f.bitangent) == doctest::Approx(1.0f));
    REQUIRE(dot(f.tangent, f.normal) == doctest::Approx(0.0f));
    REQUIRE(dot(f.tangent, f.bitangent) == doctest::Approx(0.0f));
}

TEST_CASE("tangent frame handles degenerate UVs without NaNs") {
    const bake::TangentFrame f = bake::tangentFrame({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, Vec2{0, 0},
                                                    Vec2{0, 0}, Vec2{0, 0}, Vec3{0, 0, 1});
    REQUIRE(std::isfinite(f.tangent.x));
    REQUIRE(length(f.tangent) == doctest::Approx(1.0f));
    REQUIRE(dot(f.tangent, f.normal) == doctest::Approx(0.0f));
}
