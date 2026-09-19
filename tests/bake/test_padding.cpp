#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace bake = cyber::bake;

// UV border padding (surface-baking spec: "Bake output padding across UV island
// borders"). The point of the stage is that the band CONTINUES the surface: a
// band that merely repeats the edge texel is a flat plateau whose boundary is a
// step, and a mip chain averages that step into the hard ring the cheap
// dilation every other baker ships is known for. Most of what is asserted here
// is therefore about the SHAPE of the band, not merely its presence.
namespace {

constexpr int kSize = 64;
// The quarter chart covers image columns 0..31 and rows 32..63 (the rasteriser
// flips v), so the band to its right starts at column 32.
constexpr int kChartMaxX = 31;
constexpr int kChartMinY = 32;
// A row deep inside the chart, far from the chart's own top and bottom edges,
// so the band beside it is fed only by the horizontal gradient.
constexpr int kProbeRow = 48;

// The EditMesh: one quad at z = 0 over [0,1]^2 whose UVs occupy only the
// LOWER-LEFT QUARTER of the layout, so three quarters of the image is
// background for the band to grow into.
Mesh quarterChart() {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : mesh.faceLoops(FaceId{fi})) {
            const Vec3 pos = mesh.position(mesh.loopVertex(l));
            uv[l.value] = {pos.x * 0.5f, pos.y * 0.5f};
        }
    }
    return mesh;
}

// A flat Target overhanging the EditMesh, so no cage ray at the chart border
// misses for want of surface.
Mesh flatTarget() {
    const std::vector<Vec3> p = {
        {-0.2f, -0.2f, 0}, {1.2f, -0.2f, 0}, {1.2f, 1.2f, 0}, {-0.2f, 1.2f, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, f);
}

// A CURVED Target: an n x n grid over [-0.2, 1.2]^2 with z = amplitude * a
// smooth bump. Its surface normal turns across the surface, which is what makes
// an extrapolated normal a NON-unit vector -- over a flat Target every
// direction is the same one and a renormalization test would pass vacuously.
Mesh curvedTarget(int n, float amplitude) {
    std::vector<Vec3> p;
    p.reserve(static_cast<std::size_t>(n + 1) * static_cast<std::size_t>(n + 1));
    const auto coord = [n](int i) {
        return -0.2f + 1.4f * static_cast<float>(i) / static_cast<float>(n);
    };
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float x = coord(i);
            const float y = coord(j);
            p.push_back({x, y, amplitude * std::sin(3.0f * x) * std::sin(2.5f * y)});
        }
    }
    std::vector<std::vector<Index>> f;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Index a = static_cast<Index>(j * (n + 1) + i);
            f.push_back({a, a + 1, a + static_cast<Index>(n + 2), a + static_cast<Index>(n + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// A Target of two disjoint quads carrying two material ids, split at x = 0.5.
Mesh splitIdTarget() {
    const std::vector<Vec3> p = {
        {-0.2f, -0.2f, 0}, {0.5f, -0.2f, 0}, {0.5f, 1.2f, 0}, {-0.2f, 1.2f, 0},
        {0.5f, -0.2f, 0},  {1.2f, -0.2f, 0}, {1.2f, 1.2f, 0}, {0.5f, 1.2f, 0},
    };
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& ids = mesh.faceAttributes().create<std::int32_t>("material_id");
    ids[0] = 7;
    ids[1] = 42;
    return mesh;
}

bake::BakeParams params(int paddingRadius, float cage = 0.1f) {
    bake::BakeParams p;
    p.width = kSize;
    p.height = kSize;
    p.cageDistance = cage;
    p.paddingRadius = paddingRadius;
    return p;
}

// The texels the padding stage WROTE, found by differencing the same bake with
// and without padding. That is the honest definition of the band -- it needs no
// second copy of the rasteriser's coverage rule -- and it doubles as the
// evidence that padding touches nothing else.
std::vector<std::pair<int, int>> paddedTexels(const bake::Image& plain, const bake::Image& padded) {
    std::vector<std::pair<int, int>> out;
    for (int y = 0; y < plain.height; ++y) {
        for (int x = 0; x < plain.width; ++x) {
            for (int c = 0; c < plain.channels; ++c) {
                if (plain.at(x, y, c) != padded.at(x, y, c)) {
                    out.emplace_back(x, y);
                    break;
                }
            }
        }
    }
    return out;
}

// A tilted plane as a field, so the field-sampled path has a normal that TURNS
// across the surface (a flat z = 0 field would make every extrapolated normal
// the same one and the case below vacuous).
class TiltedPlaneField final : public bake::FieldEvaluator {
public:
    [[nodiscard]] float distance(Vec3 p) const override { return (p.z - 0.15f * p.x) * kScale; }
    [[nodiscard]] Vec3 gradient(Vec3 p) const override {
        // A gentle twist, so the direction is not constant.
        return cyber::normalized(Vec3{-0.15f - 0.2f * p.y, -0.2f * p.x, 1.0f});
    }
    [[nodiscard]] float openness(Vec3, Vec3, float) const override { return 1.0f; }

private:
    // The field must never OVERESTIMATE the distance; the tilt makes the plain
    // difference slightly larger than the true distance, so it is scaled down.
    static constexpr float kScale = 0.9f;
};

std::array<int, 3> byteAt(const bake::Image& img, int px, int py) {
    std::array<int, 3> out{};
    for (int c = 0; c < 3; ++c) {
        const float v = std::clamp(img.at(px, py, c), 0.0f, 1.0f);
        out[static_cast<std::size_t>(c)] = static_cast<int>(std::lround(v * 255.0f));
    }
    return out;
}

}  // namespace

// ---- the headline behaviour -----------------------------------------------

TEST_CASE("padded texels continue a gradient running off an island") {
    // A POSITION map over the quarter chart ramps its red channel linearly
    // across the island: the model-unit x of the texel's hit, 0 at column 0 and
    // ~1 at column 31. A band that repeated the edge would hold column 31's
    // value all the way out; a band that CONTINUES the ramp keeps climbing at
    // the ramp's own slope, and that difference is the whole issue.
    const Mesh low = quarterChart();
    const Mesh high = flatTarget();
    const bake::BakeResult padded = bake::bake(low, high, bake::BakeMap::Position, params(8));
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::Position, params(0));
    REQUIRE(!padded.image.pixels.empty());
    REQUIRE(!plain.image.pixels.empty());

    const float edge = padded.image.at(kChartMaxX, kProbeRow, 0);
    // The chart spans model x in [0,1] over 32 columns.
    const float slope = 1.0f / 32.0f;
    CHECK(edge == doctest::Approx(1.0f - 0.5f * slope).epsilon(0.02));

    float previous = edge;
    for (int step = 1; step <= 8; ++step) {
        const int x = kChartMaxX + step;
        const float value = padded.image.at(x, kProbeRow, 0);
        // Strictly climbing, and not the edge value repeated.
        CHECK(value > previous);
        CHECK(value > edge + 0.5f * slope);
        // The ramp's own slope, continued. A copy-outward dilation fails the
        // first of these; a band that merely "increases somehow" fails this.
        CHECK(value == doctest::Approx(edge + static_cast<float>(step) * slope).epsilon(0.05));
        previous = value;
    }

    // Without padding the same texels hold the map's background.
    for (int step = 1; step <= 8; ++step) {
        CHECK(plain.image.at(kChartMaxX + step, kProbeRow, 0) == 0.0f);
    }
    CHECK(padded.padding.radius == 8);
    CHECK(padded.padding.mode == bake::PaddingMode::Extrapolate);
    CHECK(padded.padding.texelsFilled > 0);
}

TEST_CASE("a normal map's padded band decodes to unit directions") {
    const Mesh low = quarterChart();
    const Mesh high = curvedTarget(24, 0.05f);
    for (const bake::BakeMap map : {bake::BakeMap::Normal, bake::BakeMap::ObjectNormal}) {
        const bake::BakeResult padded = bake::bake(low, high, map, params(8));
        const bake::BakeResult plain = bake::bake(low, high, map, params(0));
        REQUIRE(!padded.image.pixels.empty());
        CHECK(padded.padding.mode == bake::PaddingMode::ExtrapolateUnit);

        const std::vector<std::pair<int, int>> band = paddedTexels(plain.image, padded.image);
        REQUIRE(band.size() > 100);
        float worst = 0.0f;
        for (const std::pair<int, int>& at : band) {
            const Vec3 decoded{padded.image.at(at.first, at.second, 0) * 2.0f - 1.0f,
                               padded.image.at(at.first, at.second, 1) * 2.0f - 1.0f,
                               padded.image.at(at.first, at.second, 2) * 2.0f - 1.0f};
            worst = std::fmax(worst, std::fabs(cyber::length(decoded) - 1.0f));
        }
        CHECK(worst < 1e-4f);
    }
}

TEST_CASE("the renormalization is real work, not a property the extrapolation already had") {
    // Guards the case above against passing vacuously: on this Target the raw
    // extrapolation of a normal map is measurably NOT unit length, so the unit
    // band above is the renormalization doing something.
    const Mesh low = quarterChart();
    const Mesh high = curvedTarget(24, 0.05f);
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::ObjectNormal, params(0));
    REQUIRE(!plain.image.pixels.empty());
    // Two adjacent columns inside the chart, differing enough that continuing
    // the gradient one step leaves the unit sphere.
    const Vec3 a{plain.image.at(kChartMaxX, kProbeRow, 0) * 2.0f - 1.0f,
                 plain.image.at(kChartMaxX, kProbeRow, 1) * 2.0f - 1.0f,
                 plain.image.at(kChartMaxX, kProbeRow, 2) * 2.0f - 1.0f};
    const Vec3 b{plain.image.at(kChartMaxX - 1, kProbeRow, 0) * 2.0f - 1.0f,
                 plain.image.at(kChartMaxX - 1, kProbeRow, 1) * 2.0f - 1.0f,
                 plain.image.at(kChartMaxX - 1, kProbeRow, 2) * 2.0f - 1.0f};
    const Vec3 extrapolated = a * 2.0f - b;
    CHECK(std::fabs(cyber::length(extrapolated) - 1.0f) > 1e-5f);
}

TEST_CASE("a scalar map is extrapolated and not renormalized") {
    // Renormalizing a single channel would drive every padded texel to exactly
    // 0 or 1 (the only |v*2-1| == 1 values there are), which is what this case
    // rules out. Displacement over a curved Target is a small, varying,
    // strictly interior value.
    const Mesh low = quarterChart();
    const Mesh high = curvedTarget(24, 0.05f);
    const bake::BakeResult padded = bake::bake(low, high, bake::BakeMap::Displacement, params(8));
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::Displacement, params(0));
    REQUIRE(padded.image.channels == 1);
    CHECK(padded.padding.mode == bake::PaddingMode::Extrapolate);

    const std::vector<std::pair<int, int>> band = paddedTexels(plain.image, padded.image);
    REQUIRE(band.size() > 100);
    std::set<int> distinct;
    for (const std::pair<int, int>& at : band) {
        const float v = padded.image.at(at.first, at.second, 0);
        // A renormalized single channel could only ever be 0 or 1 -- the two
        // values with |v*2-1| == 1. These stay at the Target's own signed
        // displacement scale instead.
        CHECK(v != 1.0f);
        CHECK(std::fabs(v) < 0.3f);
        distinct.insert(static_cast<int>(std::lround(v * 10000.0f)));
    }
    // Varying, so the band is the map continued rather than one constant, and
    // so it is not the two-valued set a renormalization would leave.
    CHECK(distinct.size() > 4);
}

TEST_CASE("an id map's padded band holds exact id colours, never a blend") {
    const Mesh low = quarterChart();
    const Mesh high = splitIdTarget();
    const bake::BakeResult padded = bake::bake(low, high, bake::BakeMap::MaterialId, params(8));
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::MaterialId, params(0));
    REQUIRE(!padded.image.pixels.empty());
    CHECK(padded.padding.mode == bake::PaddingMode::Nearest);
    REQUIRE(padded.encoding.idColors.size() == 2);

    std::set<std::array<int, 3>> allowed;
    for (const bake::IdColorEntry& entry : padded.encoding.idColors) {
        allowed.insert({entry.color[0], entry.color[1], entry.color[2]});
    }
    const std::vector<std::pair<int, int>> band = paddedTexels(plain.image, padded.image);
    REQUIRE(band.size() > 100);
    for (const std::pair<int, int>& at : band) {
        // The 8-bit triple a consumer actually picks, compared at ZERO
        // tolerance: an interpolated id colour resolves to no row of the table.
        CHECK(allowed.count(byteAt(padded.image, at.first, at.second)) == 1);
        // And exactly, in the float buffer, not merely after rounding.
        for (const bake::IdColorEntry& entry : padded.encoding.idColors) {
            const float expected = static_cast<float>(entry.color[0]) / 255.0f;
            if (padded.image.at(at.first, at.second, 0) == expected) {
                for (int c = 1; c < 3; ++c) {
                    CHECK(padded.image.at(at.first, at.second, c) ==
                          static_cast<float>(entry.color[static_cast<std::size_t>(c)]) / 255.0f);
                }
                break;
            }
        }
    }
}

// ---- the radius -----------------------------------------------------------

TEST_CASE("radius zero returns the map exactly as it was baked") {
    const Mesh low = quarterChart();
    const Mesh high = curvedTarget(24, 0.05f);
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::Normal, params(0));
    REQUIRE(!plain.image.pixels.empty());
    CHECK(plain.padding.radius == 0);
    CHECK(plain.padding.mode == bake::PaddingMode::None);
    CHECK(plain.padding.texelsFilled == 0);
    // Nothing outside the chart was touched: the background is still the
    // pre-fill everywhere.
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (x <= kChartMaxX && y >= kChartMinY) {
                continue;
            }
            CHECK(plain.image.at(x, y, 0) == 0.5f);
            CHECK(plain.image.at(x, y, 1) == 0.5f);
            CHECK(plain.image.at(x, y, 2) == 1.0f);
        }
    }
}

TEST_CASE("a negative padding radius is refused rather than defaulted") {
    const Mesh low = quarterChart();
    const Mesh high = flatTarget();
    const bake::BakeResult result = bake::bake(low, high, bake::BakeMap::Normal, params(-1));
    CHECK(result.image.pixels.empty());
    CHECK(result.texelsCovered == 0);
}

TEST_CASE("the band is at most `radius` texels wide") {
    const Mesh low = quarterChart();
    const Mesh high = flatTarget();
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::Position, params(0));
    for (const int radius : {2, 8}) {
        const bake::BakeResult padded =
            bake::bake(low, high, bake::BakeMap::Position, params(radius));
        REQUIRE(!padded.image.pixels.empty());
        for (const std::pair<int, int>& at : paddedTexels(plain.image, padded.image)) {
            // Chebyshev distance to the chart, which spans columns [0, 31] and
            // rows [32, 63].
            const int dx = std::max(0, at.first - kChartMaxX);
            const int dy = std::max(0, kChartMinY - at.second);
            CHECK(std::max(dx, dy) <= radius);
        }
        // The band actually REACHES its radius, so the bound above is not
        // passing on an empty or stunted band.
        CHECK(padded.image.at(kChartMaxX + radius, kProbeRow, 0) != 0.0f);
        CHECK(padded.image.at(kChartMaxX + radius + 1, kProbeRow, 0) == 0.0f);
    }
}

// ---- the guarantees -------------------------------------------------------

TEST_CASE("a continuation cannot compound past one range's width") {
    // A ring reads the ring before it, so a map that is not locally linear
    // compounds. The bound is the covered range widened by its own width.
    const Mesh low = quarterChart();
    const Mesh high = curvedTarget(40, 0.08f);
    const bake::BakeResult plain = bake::bake(low, high, bake::BakeMap::Displacement, params(0));
    const bake::BakeResult padded = bake::bake(low, high, bake::BakeMap::Displacement, params(8));
    REQUIRE(!padded.image.pixels.empty());

    float low_ = 1e30f;
    float high_ = -1e30f;
    for (int y = kChartMinY; y < kSize; ++y) {
        for (int x = 0; x <= kChartMaxX; ++x) {
            low_ = std::fmin(low_, plain.image.at(x, y, 0));
            high_ = std::fmax(high_, plain.image.at(x, y, 0));
        }
    }
    const float span = high_ - low_;
    REQUIRE(span > 0.0f);
    for (const std::pair<int, int>& at : paddedTexels(plain.image, padded.image)) {
        const float v = padded.image.at(at.first, at.second, 0);
        CHECK(v >= low_ - span - 1e-6f);
        CHECK(v <= high_ + span + 1e-6f);
    }
}

TEST_CASE("padding is deterministic") {
    const Mesh low = quarterChart();
    const Mesh high = curvedTarget(24, 0.05f);
    for (const bake::BakeMap map :
         {bake::BakeMap::Normal, bake::BakeMap::Position, bake::BakeMap::MaterialId}) {
        const bake::BakeResult first = bake::bake(low, high, map, params(8));
        const bake::BakeResult second = bake::bake(low, high, map, params(8));
        REQUIRE(first.image.pixels.size() == second.image.pixels.size());
        CHECK(first.image.pixels == second.image.pixels);
        CHECK(first.padding.texelsFilled == second.padding.texelsFilled);
    }
}

TEST_CASE("padding is cancellable and leaves an abandoned bake alone") {
    const Mesh low = quarterChart();
    const Mesh high = flatTarget();
    // Cancelled before the first ring: the bake reports itself cancelled and
    // the caller keeps its previous maps, exactly as a cancelled shade does.
    const cyber::CancelToken cancel;
    cancel.setPoll([]() { return true; });
    const bake::BakeResult cancelled =
        bake::bake(low, high, bake::BakeMap::Position, params(8), nullptr, &cancel);
    CHECK(cancelled.cancelled);
}

TEST_CASE("every map reports a padding record") {
    const Mesh low = quarterChart();
    const Mesh high = splitIdTarget();
    const std::vector<std::pair<bake::BakeMap, bake::PaddingMode>> expected = {
        {bake::BakeMap::Normal, bake::PaddingMode::ExtrapolateUnit},
        {bake::BakeMap::ObjectNormal, bake::PaddingMode::ExtrapolateUnit},
        {bake::BakeMap::AmbientOcclusion, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::Displacement, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::Position, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::ObjectPosition, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::Color, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::Curvature, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::Cavity, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::Thickness, bake::PaddingMode::Extrapolate},
        {bake::BakeMap::MaterialId, bake::PaddingMode::Nearest},
        {bake::BakeMap::ObjectId, bake::PaddingMode::Nearest},
    };
    for (const std::pair<bake::BakeMap, bake::PaddingMode>& entry : expected) {
        bake::BakeParams p = params(4);
        p.aoSamples = 8;
        const bake::BakeResult result = bake::bake(low, high, entry.first, p);
        REQUIRE(!result.image.pixels.empty());
        CHECK(result.padding.radius == 4);
        CHECK(result.padding.mode == entry.second);
        CHECK(result.padding.texelsFilled > 0);
    }
}

TEST_CASE("the field-sampled path is padded by the same stage") {
    // Padding runs after the shade, on the coverage the rasteriser produced, so
    // both paths get one implementation. A map that padded only on the raycast
    // path would ship a seam for every field-sampled bake.
    const Mesh low = quarterChart();
    const Mesh empty;
    const TiltedPlaneField field;
    bake::BakeParams padded = params(6, 0.5f);
    padded.field = &field;
    bake::BakeParams plainParams = padded;
    plainParams.paddingRadius = 0;

    const bake::BakeResult result = bake::bake(low, empty, bake::BakeMap::Normal, padded);
    const bake::BakeResult plain = bake::bake(low, empty, bake::BakeMap::Normal, plainParams);
    REQUIRE(!result.image.pixels.empty());
    REQUIRE(!result.fieldContractViolated);
    CHECK(result.padding.radius == 6);
    CHECK(result.padding.mode == bake::PaddingMode::ExtrapolateUnit);
    CHECK(result.padding.texelsFilled > 0);

    const std::vector<std::pair<int, int>> band = paddedTexels(plain.image, result.image);
    REQUIRE(!band.empty());
    for (const std::pair<int, int>& at : band) {
        const Vec3 decoded{result.image.at(at.first, at.second, 0) * 2.0f - 1.0f,
                           result.image.at(at.first, at.second, 1) * 2.0f - 1.0f,
                           result.image.at(at.first, at.second, 2) * 2.0f - 1.0f};
        CHECK(std::fabs(cyber::length(decoded) - 1.0f) < 1e-4f);
    }
}

TEST_CASE("a bake that covers the whole layout pads nothing") {
    // No background to grow into: the band is empty and the reported mode says
    // so, while the radius still reports what was asked for.
    Mesh low = quarterChart();
    std::vector<Vec2>* uv = low.cornerAttributes().find<Vec2>("uv");
    for (Vec2& coord : *uv) {
        coord = {coord.x * 2.0f, coord.y * 2.0f};
    }
    const bake::BakeResult result =
        bake::bake(low, flatTarget(), bake::BakeMap::Position, params(8));
    REQUIRE(!result.image.pixels.empty());
    CHECK(result.padding.radius == 8);
    CHECK(result.padding.texelsFilled == 0);
    CHECK(result.padding.mode == bake::PaddingMode::None);
}
