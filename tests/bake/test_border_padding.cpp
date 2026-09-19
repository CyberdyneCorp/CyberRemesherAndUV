#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "border_padding.hpp"
#include "cyber/core/progress.hpp"

// The UV border padding stage, tested DIRECTLY on images the case authors
// (surface-baking spec, "Bake output padding across UV island borders").
//
// tests/bake/test_padding.cpp drives the same stage end to end through bake(),
// which is the right place for "does a real bake come out padded". It is the
// wrong place for the stage's three guarantees -- the band stays inside the
// map's own value range, the band does not depend on the order texels are
// visited in, the band honours cancellation -- because through bake() the input
// values are whatever the rasteriser and the shade happened to produce, and an
// assertion on values nobody chose is an assertion that passes for reasons
// nobody checked. Here every covered texel's value is written by the case, so
// every expected value below is arithmetic that can be done by hand.
namespace bake = cyber::bake;
namespace detail = cyber::bake::detail;

using detail::PadCoord;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

bake::Image makeImage(int width, int height, int channels, float background = 0.0f) {
    bake::Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                            static_cast<std::size_t>(channels),
                        background);
    return image;
}

// An encoding that guarantees nothing about its values, the way a position map
// in model units or a signed displacement does. Only the compounding bound
// applies to such a map.
bake::BakeEncoding unbounded(bake::EncodingBasis basis = bake::EncodingBasis::None) {
    bake::BakeEncoding encoding;
    encoding.basis = basis;
    return encoding;
}

// An encoding that guarantees [lo, hi], the way an object-space position
// guarantees [0,1] or a thickness guarantees [0, inf).
bake::BakeEncoding bounded(float lo, float hi,
                           bake::EncodingBasis basis = bake::EncodingBasis::None) {
    bake::BakeEncoding encoding = unbounded(basis);
    encoding.valueMin = lo;
    encoding.valueMax = hi;
    return encoding;
}

// A single-channel chart in the lower-left corner whose value ramps steeply
// across it, so a continuation off its right and top edges runs away fast.
// `slope` per column, starting at `base` in column 0.
std::vector<PadCoord> rampChart(bake::Image& image, int size, float base, float slope) {
    std::vector<PadCoord> covered;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            image.at(x, y, 0) = base + slope * static_cast<float>(x);
            covered.push_back(PadCoord{x, y});
        }
    }
    return covered;
}

float maxValue(const bake::Image& image) {
    float out = -kInf;
    for (const float v : image.pixels) {
        out = std::fmax(out, v);
    }
    return out;
}

float minValue(const bake::Image& image) {
    float out = kInf;
    for (const float v : image.pixels) {
        out = std::fmin(out, v);
    }
    return out;
}

}  // namespace

// ---- the band stays inside the map's own value range ----------------------

TEST_CASE("a padded band cannot leave the range its encoding guarantees") {
    // The compounding bound alone is [low - span, high + span]: for a map whose
    // covered texels span the whole of [0,1] that is [-1, 2]. It is a bound on
    // RUNAWAY, not a statement about what the map means. An object-space
    // position means `(p - min) / (max - min)` and a consumer decodes it as
    // `min + v * (max - min)`; a padded texel at 1.4 hands that consumer a point
    // outside the box the bake recorded. An occlusion above 1 is more than a
    // whole hemisphere; a thickness below 0 is a distance that ran backwards.
    constexpr int kSize = 32;
    constexpr int kChart = 12;

    // FIRST, the same input through an encoding that guarantees nothing, to
    // prove the case is not vacuous: this ramp really does run out of [0,1].
    bake::Image loose = makeImage(kSize, kSize, 1);
    const std::vector<PadCoord> covered = rampChart(loose, kChart, 0.0f, 1.0f / 11.0f);
    const detail::PadOutcome looseOut = detail::padBorders(loose, covered, unbounded(), 8, nullptr);
    REQUIRE(looseOut.padding.texelsFilled > 0);
    CHECK(maxValue(loose) > 1.0f);

    // SAME image, same coverage, same radius -- only the declared range differs.
    bake::Image strict = makeImage(kSize, kSize, 1);
    const std::vector<PadCoord> same = rampChart(strict, kChart, 0.0f, 1.0f / 11.0f);
    const detail::PadOutcome strictOut = detail::padBorders(
        strict, same, bounded(0.0f, 1.0f, bake::EncodingBasis::ObjectBounds), 8, nullptr);
    REQUIRE(strictOut.padding.texelsFilled == looseOut.padding.texelsFilled);
    CHECK(strictOut.padding.mode == bake::PaddingMode::Extrapolate);
    CHECK(maxValue(strict) <= 1.0f);
    CHECK(minValue(strict) >= 0.0f);
    // The ceiling was actually REACHED, so the clamp is what bounded it rather
    // than the ramp happening to stop short.
    CHECK(maxValue(strict) == 1.0f);
}

TEST_CASE("a one-sided range bounds only the side it declares") {
    // Thickness: zero at worst ("a ray that hits nothing, or that hits a front
    // face, SHALL contribute zero"), with no ceiling. A band that runs UP is a
    // thicker solid, which the encoding permits; a band that runs DOWN past zero
    // is not a thinner one.
    constexpr int kSize = 32;
    bake::Image image = makeImage(kSize, kSize, 1);
    // Ramps DOWN to the right, so the continuation past the chart goes negative.
    const std::vector<PadCoord> covered = rampChart(image, 12, 1.0f, -1.0f / 11.0f);
    const detail::PadOutcome out = detail::padBorders(
        image, covered, bounded(0.0f, kInf, bake::EncodingBasis::Distance), 8, nullptr);
    REQUIRE(out.padding.texelsFilled > 0);
    CHECK(minValue(image) >= 0.0f);
    CHECK(minValue(image) == 0.0f);  // the floor is reached, so it did the work
    // Nothing invented a ceiling: the band above the chart continues upward past
    // the covered maximum, which is what an unbounded top side means.
    CHECK(maxValue(image) > 1.0f);
}

// ---- the band does not depend on any visit order --------------------------

TEST_CASE("a ring reads only the state from before it") {
    // The order-independence guarantee, at the one place it can actually break:
    // WITHIN a ring. Two covered texels two apart, with different values, so
    // every ring-1 texel's value is decided by which of them it can see -- and
    // a texel that its own ring filled a moment ago would be a THIRD source.
    //
    //   . . . . . . . . .      A = (3,3) = 0.25
    //   . . . . . . . . .      B = (5,3) = 1.0
    //   . . a b c d e . .      the band below is ring 1
    //   . . f A g B h . .
    //   . . i j k l m . .
    //
    // Every value below is 2*v1 - v2 averaged over the covered compass
    // directions, with v2 collapsing to v1 wherever the second sample is
    // uncovered -- which here is everywhere, because A and B are isolated. So
    // each ring-1 texel holds the MEAN of the covered neighbours it can see:
    // 0.25 beside A only, 1.0 beside B only, 0.625 where it sees both.
    bake::Image image = makeImage(9, 7, 1);
    image.at(3, 3, 0) = 0.25f;
    image.at(5, 3, 0) = 1.0f;
    const std::vector<PadCoord> covered = {{3, 3}, {5, 3}};

    const detail::PadOutcome out = detail::padBorders(image, covered, unbounded(), 1, nullptr);
    CHECK(out.padding.texelsFilled == 13);  // 8 + 8 neighbours, 3 of them shared

    const std::vector<std::pair<PadCoord, float>> expected = {
        // Beside A alone.
        {{2, 2}, 0.25f},
        {{3, 2}, 0.25f},
        {{2, 3}, 0.25f},
        {{2, 4}, 0.25f},
        {{3, 4}, 0.25f},
        // Beside B alone.
        {{5, 2}, 1.0f},
        {{6, 2}, 1.0f},
        {{6, 3}, 1.0f},
        {{6, 4}, 1.0f},
        {{5, 4}, 1.0f},
        // The three that see BOTH. A ring that let its own output become a
        // source would give these a third and a fourth neighbour -- (3,2) and
        // (4,2) are filled EARLIER in the same ring -- and every one of them
        // would move off 0.625.
        {{4, 2}, 0.625f},
        {{4, 3}, 0.625f},
        {{4, 4}, 0.625f},
    };
    for (const std::pair<PadCoord, float>& entry : expected) {
        CHECK(image.at(entry.first.x, entry.first.y, 0) == entry.second);
    }
}

TEST_CASE("the band does not depend on the order the coverage is listed in") {
    // The rasteriser emits texels in its own order, a field path in another, and
    // a future parallel rasteriser in a third. None of them may change a texel
    // of the band.
    constexpr int kSize = 24;
    bake::Image reference = makeImage(kSize, kSize, 3);
    std::vector<PadCoord> covered;
    for (int y = 4; y < 14; ++y) {
        for (int x = 3; x < 15; ++x) {
            // Deliberately NOT linear: a linear map extrapolates to the same
            // value from any source and would hide an order dependence.
            reference.at(x, y, 0) = std::sin(0.7f * static_cast<float>(x));
            reference.at(x, y, 1) = std::cos(0.5f * static_cast<float>(y));
            reference.at(x, y, 2) = 0.01f * static_cast<float>(x * y);
            covered.push_back(PadCoord{x, y});
        }
    }
    bake::Image shuffled = reference;

    const detail::PadOutcome first =
        detail::padBorders(reference, covered, unbounded(), 6, nullptr);

    // A fixed, reproducible permutation -- reversed, then odd indices first --
    // rather than a random one, so a failure is the same failure on every run.
    std::vector<PadCoord> reordered;
    reordered.reserve(covered.size());
    for (std::size_t i = covered.size(); i-- > 0;) {
        if (i % 2 == 1) {
            reordered.push_back(covered[i]);
        }
    }
    for (std::size_t i = 0; i < covered.size(); ++i) {
        if (i % 2 == 0) {
            reordered.push_back(covered[i]);
        }
    }
    REQUIRE(reordered.size() == covered.size());
    const detail::PadOutcome second =
        detail::padBorders(shuffled, reordered, unbounded(), 6, nullptr);

    CHECK(first.padding.texelsFilled == second.padding.texelsFilled);
    CHECK(reference.pixels == shuffled.pixels);  // bit for bit, not within a tolerance
}

// ---- cancellation ---------------------------------------------------------

TEST_CASE("padding stops when the token trips and leaves the rest of the image alone") {
    constexpr int kSize = 32;
    constexpr int kChart = 8;
    constexpr int kRingsBeforeCancel = 3;

    bake::Image image = makeImage(kSize, kSize, 1, -1.0f);
    const std::vector<PadCoord> covered = rampChart(image, kChart, 0.0f, 0.05f);

    // Cancelled between rings, not before the stage: the band written so far
    // stays, and every texel the remaining rings would have reached is still the
    // background the image came in with.
    int polls = 0;
    const cyber::CancelToken cancel;
    cancel.setPoll([&polls]() { return polls++ >= kRingsBeforeCancel; });
    const detail::PadOutcome out = detail::padBorders(image, covered, unbounded(), 8, &cancel);

    CHECK(out.cancelled);
    CHECK(out.padding.radius == 8);
    CHECK(out.padding.texelsFilled > 0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            // Chebyshev distance to the chart, which spans [0, kChart-1]^2.
            const int d = std::max(std::max(0, x - (kChart - 1)), std::max(0, y - (kChart - 1)));
            if (d > kRingsBeforeCancel) {
                CHECK(image.at(x, y, 0) == -1.0f);
            }
        }
    }
}

TEST_CASE("a token already cancelled writes no band at all") {
    constexpr int kSize = 16;
    bake::Image image = makeImage(kSize, kSize, 1, -1.0f);
    const bake::Image before = image;
    const std::vector<PadCoord> covered = rampChart(image, 6, 0.0f, 0.05f);
    const bake::Image seeded = image;

    const cyber::CancelToken cancel;
    cancel.requestCancel();
    const detail::PadOutcome out = detail::padBorders(image, covered, unbounded(), 8, &cancel);

    CHECK(out.cancelled);
    CHECK(out.padding.texelsFilled == 0);
    CHECK(out.padding.mode == bake::PaddingMode::None);
    CHECK(image.pixels == seeded.pixels);  // only the chart the case wrote
    CHECK(before.pixels != seeded.pixels);
}

// ---- the fill rule --------------------------------------------------------

TEST_CASE("the fill rule is read off the encoding basis") {
    CHECK(detail::paddingModeFor(bake::EncodingBasis::IdColor) == bake::PaddingMode::Nearest);
    CHECK(detail::paddingModeFor(bake::EncodingBasis::TangentNormal) ==
          bake::PaddingMode::ExtrapolateUnit);
    CHECK(detail::paddingModeFor(bake::EncodingBasis::ObjectNormal) ==
          bake::PaddingMode::ExtrapolateUnit);
    CHECK(detail::paddingModeFor(bake::EncodingBasis::None) == bake::PaddingMode::Extrapolate);
    CHECK(detail::paddingModeFor(bake::EncodingBasis::Distance) == bake::PaddingMode::Extrapolate);
    // ObjectBounds continues its gradient like any other scalar map; what keeps
    // it inside [0,1] is the range its encoding declares, not a separate rule.
    CHECK(detail::paddingModeFor(bake::EncodingBasis::ObjectBounds) ==
          bake::PaddingMode::Extrapolate);
}

TEST_CASE("a map with more channels than the padder can carry is left unpadded") {
    // PadTexel is a fixed four-channel array, chosen so the stage allocates
    // nothing per texel. A five-channel map would be read and written past its
    // end. Refusing the stage ships the map visibly unpadded -- mode None, zero
    // texels -- instead of corrupting it silently.
    constexpr int kSize = 12;
    bake::Image image = makeImage(kSize, kSize, 5, 0.0f);
    std::vector<PadCoord> covered;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            for (int c = 0; c < 5; ++c) {
                image.at(x, y, c) = 0.1f * static_cast<float>(c + x);
            }
            covered.push_back(PadCoord{x, y});
        }
    }
    const bake::Image before = image;
    const detail::PadOutcome out = detail::padBorders(image, covered, unbounded(), 4, nullptr);
    CHECK(out.padding.texelsFilled == 0);
    CHECK(out.padding.mode == bake::PaddingMode::None);
    CHECK(!out.cancelled);
    CHECK(image.pixels == before.pixels);
}
