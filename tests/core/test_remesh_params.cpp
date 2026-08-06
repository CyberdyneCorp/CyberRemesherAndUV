#include <doctest.h>

#include <cmath>
#include <limits>
#include <string>

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

// ---------------------------------------------------------------------------
// Guidance validation (remeshing-parameters spec, "Guide and density
// parameters are validated").
// ---------------------------------------------------------------------------

namespace {

remesh::FlowGuide sampleGuide() {
    remesh::FlowGuide g;
    g.points = {cyber::Vec3{0, 0, 0}, cyber::Vec3{1, 0, 0}};
    g.strength = 1.0f;
    g.radius = 0.5f;
    return g;
}

bool hasFatal(const remesh::ValidatedGuidance& v, const std::string& parameter) {
    for (const auto& issue : v.issues) {
        if (issue.fatal && issue.parameter == parameter) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("empty guidance validates with no issues") {
    const auto validated = remesh::validateGuidance(remesh::Guidance{}, 10, 8);
    REQUIRE(validated.ok());
    REQUIRE(validated.issues.empty());
    REQUIRE(validated.guidance.empty());
}

TEST_CASE("guide strength out of range clamps and the effective value is reported") {
    remesh::Guidance g;
    remesh::FlowGuide guide = sampleGuide();
    guide.strength = 5.0f;
    g.guides.push_back(guide);

    const auto validated = remesh::validateGuidance(g, 10, 8);
    REQUIRE(validated.ok());  // a clamp is a warning, not a rejection
    REQUIRE(validated.issues.size() == 1);
    CHECK_FALSE(validated.issues[0].fatal);
    CHECK(validated.issues[0].parameter == "guides[0].strength");
    CHECK(validated.issues[0].message.find("5.0") != std::string::npos);  // original
    CHECK(validated.issues[0].message.find("1.0") != std::string::npos);  // clamped
    REQUIRE(validated.guidance.guides.size() == 1);
    CHECK(validated.guidance.guides[0].strength == doctest::Approx(remesh::kGuideStrengthMax));

    remesh::Guidance low;
    remesh::FlowGuide negative = sampleGuide();
    negative.strength = -2.0f;
    low.guides.push_back(negative);
    const auto clampedLow = remesh::validateGuidance(low, 10, 8);
    REQUIRE(clampedLow.ok());
    CHECK(clampedLow.guidance.guides[0].strength == doctest::Approx(remesh::kGuideStrengthMin));
}

TEST_CASE("a guide with fewer than two points is rejected, not dropped") {
    remesh::Guidance g;
    remesh::FlowGuide guide;
    guide.points = {cyber::Vec3{0, 0, 0}};
    guide.radius = 0.5f;
    g.guides.push_back(guide);

    const auto validated = remesh::validateGuidance(g, 10, 8);
    CHECK_FALSE(validated.ok());
    CHECK(hasFatal(validated, "guides[0]"));
    CHECK(validated.issues[0].message.find("at least 2") != std::string::npos);
}

TEST_CASE("a zero or negative guide radius is fatal") {
    for (const float radius : {0.0f, -1.0f}) {
        remesh::Guidance g;
        remesh::FlowGuide guide = sampleGuide();
        guide.radius = radius;
        g.guides.push_back(guide);
        const auto validated = remesh::validateGuidance(g, 10, 8);
        CHECK_FALSE(validated.ok());
        CHECK(hasFatal(validated, "guides[0]"));
    }
}

TEST_CASE("non-finite guide values are fatal") {
    SUBCASE("a point") {
        remesh::Guidance g;
        remesh::FlowGuide guide = sampleGuide();
        guide.points[1].y = std::nanf("");
        g.guides.push_back(guide);
        const auto validated = remesh::validateGuidance(g, 10, 8);
        CHECK_FALSE(validated.ok());
        CHECK(hasFatal(validated, "guides[0]"));
    }
    SUBCASE("the radius") {
        remesh::Guidance g;
        remesh::FlowGuide guide = sampleGuide();
        guide.radius = std::numeric_limits<float>::infinity();
        g.guides.push_back(guide);
        const auto validated = remesh::validateGuidance(g, 10, 8);
        CHECK_FALSE(validated.ok());
    }
    SUBCASE("the strength") {
        remesh::Guidance g;
        remesh::FlowGuide guide = sampleGuide();
        guide.strength = std::nanf("");
        g.guides.push_back(guide);
        const auto validated = remesh::validateGuidance(g, 10, 8);
        CHECK_FALSE(validated.ok());
        CHECK(hasFatal(validated, "guides[0].strength"));
    }
}

TEST_CASE("density values clamp to the documented range with an aggregated issue") {
    remesh::Guidance g;
    g.density.vertexValues = {1.0f, 100.0f, 0.001f, 2.0f};
    const auto validated = remesh::validateGuidance(g, 4, 2);
    REQUIRE(validated.ok());
    REQUIRE(validated.issues.size() == 1);  // aggregated, not one issue per value
    CHECK_FALSE(validated.issues[0].fatal);
    CHECK(validated.issues[0].parameter == "density");
    CHECK(validated.issues[0].message.find("2 value(s)") != std::string::npos);
    CHECK(validated.guidance.density.vertexValues[1] == doctest::Approx(remesh::kDensityMax));
    CHECK(validated.guidance.density.vertexValues[2] == doctest::Approx(remesh::kDensityMin));
    CHECK(validated.guidance.density.vertexValues[0] == doctest::Approx(1.0f));
}

TEST_CASE("a density array of the wrong length is fatal") {
    SUBCASE("per-vertex") {
        remesh::Guidance g;
        g.density.vertexValues = {1.0f, 1.0f, 1.0f};
        const auto validated = remesh::validateGuidance(g, 4, 2);
        CHECK_FALSE(validated.ok());
        CHECK(hasFatal(validated, "density"));
    }
    SUBCASE("per-face") {
        remesh::Guidance g;
        g.density.faceValues = {1.0f, 1.0f, 1.0f};
        const auto validated = remesh::validateGuidance(g, 4, 2);
        CHECK_FALSE(validated.ok());
        CHECK(hasFatal(validated, "density"));
    }
    SUBCASE("both arrays supplied") {
        remesh::Guidance g;
        g.density.vertexValues = {1.0f, 1.0f, 1.0f, 1.0f};
        g.density.faceValues = {1.0f, 1.0f};
        const auto validated = remesh::validateGuidance(g, 4, 2);
        CHECK_FALSE(validated.ok());
        CHECK(hasFatal(validated, "density"));
    }
}

TEST_CASE("a non-finite density value is fatal") {
    remesh::Guidance g;
    g.density.faceValues = {1.0f, std::nanf("")};
    const auto validated = remesh::validateGuidance(g, 4, 2);
    CHECK_FALSE(validated.ok());
    CHECK(hasFatal(validated, "density"));
}
