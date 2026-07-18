#include <doctest.h>

#include <cmath>

#include "cyber/core/remesh_params.hpp"

namespace remesh = cyber::remesh;

TEST_CASE("default parameters validate with no issues") {
    const auto validated = remesh::validate(remesh::Parameters{});
    REQUIRE(validated.ok());
    REQUIRE(validated.issues.empty());
    REQUIRE(validated.params.targetQuadCount == 50'000);
    REQUIRE(validated.params.sharpEdgeDegrees == doctest::Approx(90.0f));
}

TEST_CASE("out-of-range values clamp with a warning naming the parameter (spec)") {
    remesh::Parameters p;
    p.edgeScale = 10.0f;  // the AutoRemesher CLI passed this through silently
    const auto validated = remesh::validate(p);
    REQUIRE(validated.ok());  // clamp is a warning, not fatal
    REQUIRE(validated.params.edgeScale == doctest::Approx(4.0f));
    REQUIRE(validated.issues.size() == 1);
    REQUIRE(validated.issues[0].parameter == "edgeScale");
    REQUIRE(validated.issues[0].message.find("10.0") != std::string::npos);
    REQUIRE(validated.issues[0].message.find("4.0") != std::string::npos);
}

TEST_CASE("non-finite values are fatal") {
    remesh::Parameters p;
    p.adaptivity = std::nanf("");
    const auto validated = remesh::validate(p);
    REQUIRE(!validated.ok());
}

TEST_CASE("zero target quads is an error, not a division by zero (spec)") {
    const auto result = remesh::targetEdgeLength(10.0, 0, 1.0f);
    REQUIRE(!result.ok());
    REQUIRE(result.error.find("positive") != std::string::npos);
}

TEST_CASE("zero surface area is an error") {
    REQUIRE(!remesh::targetEdgeLength(0.0, 1000, 1.0f).ok());
    REQUIRE(!remesh::targetEdgeLength(-5.0, 1000, 1.0f).ok());
}

TEST_CASE("target edge length matches the equilateral derivation") {
    // 1000 quads over area 1000 -> 2000 triangles of area 0.5 each;
    // equilateral side = sqrt(0.5 / 0.43301...) ~ 1.0746.
    const auto result = remesh::targetEdgeLength(1000.0, 1000, 1.0f);
    REQUIRE(result.ok());
    REQUIRE(result.edgeLength == doctest::Approx(1.0746f).epsilon(0.001));
    // edgeScale multiplies through.
    const auto scaled = remesh::targetEdgeLength(1000.0, 1000, 2.0f);
    REQUIRE(scaled.edgeLength == doctest::Approx(2.0f * result.edgeLength));
}
