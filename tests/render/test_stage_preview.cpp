#include <doctest.h>

#include <string>
#include <vector>

#include "cyber/render/stage_preview.hpp"

using namespace cyber;
using namespace cyber::render;

TEST_CASE("stage names round-trip to strings") {
    CHECK(std::string(toString(PreviewStage::Source)) == "Source");
    CHECK(std::string(toString(PreviewStage::Isotropic)) == "Isotropic");
    CHECK(std::string(toString(PreviewStage::Param)) == "Param");
    CHECK(std::string(toString(PreviewStage::Result)) == "Result");
}

TEST_CASE("singularity classification and coloring by valence") {
    CHECK_FALSE(isSingular(4));
    CHECK(isSingular(3));
    CHECK(isSingular(5));

    const Vec4 regular = singularityColor(4);
    CHECK(regular.x == doctest::Approx(regular.z));  // grey: r == b

    const Vec4 low = singularityColor(3);
    CHECK(low.z > low.x);  // blue dominates for low valence

    const Vec4 high = singularityColor(5);
    CHECK(high.x > high.z);  // red dominates for high valence
}

TEST_CASE("cross field expands to four arms per sample") {
    std::vector<CrossFieldSample> samples = {
        CrossFieldSample{Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}},
        CrossFieldSample{Vec3{5, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}},
    };
    const std::vector<std::pair<Vec3, Vec3>> segs = crossFieldSegments(samples, 0.5f);
    CHECK(segs.size() == 4);  // 2 line segments (4 arms) per sample
    // First segment spans +/- dir0 * length about the position.
    CHECK(segs[0].first.x == doctest::Approx(-0.5f));
    CHECK(segs[0].second.x == doctest::Approx(0.5f));
}

TEST_CASE("cross pattern produces a lattice of iso-lines") {
    CrossPatternPreview pattern;
    pattern.linesU = 5;
    pattern.linesV = 3;
    const std::vector<std::pair<Vec2, Vec2>> segs = crossPatternSegments(pattern);
    CHECK(segs.size() == 8);  // 5 vertical + 3 horizontal
}

TEST_CASE("default descriptors preset stage-specific toggles") {
    const StagePreviewDescriptor param = defaultDescriptor(PreviewStage::Param);
    CHECK(static_cast<int>(param.stage) == static_cast<int>(PreviewStage::Param));
    CHECK(param.showCrossField);
    CHECK(param.showSingularities);

    const StagePreviewDescriptor source = defaultDescriptor(PreviewStage::Source);
    CHECK(source.showWireframe);
    CHECK_FALSE(source.showCrossField);
}
