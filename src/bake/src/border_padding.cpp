#include "border_padding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// A baked map stops at the edge of each UV island, and everything downstream --
// a bilinear tap, a mip level, a BC block -- reaches past that edge and mixes
// the outermost covered texel with the background. Padding fills a band outside
// each island so those reads land on baked values instead.
//
// The band CONTINUES the gradient running off the island rather than repeating
// its edge value. A repeated edge is a flat plateau whose boundary is a step,
// and a mip chain averages that step into the visible hard ring every cheap
// dilation ships; a continuation has no step to average. ArmorPaint's
// dilate_pass.kong:64-112 (zlib) is the reference.
namespace cyber::bake::detail {

namespace {

struct PadOffset {
    int dx;
    int dy;
};

// N, NE, E, SE, S, SW, W, NW. The extrapolating rule averages over every
// direction that hits, so the order cannot reach it; the id rule takes the
// first that hits and needs one that is fixed.
constexpr std::array<PadOffset, 8> kCompass{
    {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};

// Image::channels is 1 or 3 for every map; 4 leaves room without a heap
// allocation per texel. A map with more than this is refused outright by the
// padder rather than indexed out of bounds -- see BorderPadder's constructor.
constexpr std::size_t kMaxChannels = 4;
using PadTexel = std::array<float, kMaxChannels>;

// Coverage states. `Queued` exists so a ring reads only the state from BEFORE
// it: a texel filled by the ring in progress is not a source for its siblings,
// which is what makes the band independent of the order candidates are visited
// in -- and therefore identical on every toolchain.
enum class PadState : std::uint8_t {
    Empty = 0,
    Covered = 1,
    Queued = 2,
};

class BorderPadder {
public:
    BorderPadder(Image& image, const std::vector<PadCoord>& covered, const BakeEncoding& encoding)
        : image_(image),
          channels_(static_cast<std::size_t>(image.channels)),
          state_(pixelCount(image), PadState::Empty) {
        // A map with more channels than PadTexel holds would be written out of
        // bounds by read()/write(). Refuse the whole stage instead: the map
        // ships unpadded and reports PaddingMode::None, which is visibly wrong
        // rather than silently corrupt.
        if (channels_ == 0 || channels_ > kMaxChannels) {
            return;
        }
        usable_ = true;
        for (const PadCoord& tx : covered) {
            state_[index(tx.x, tx.y)] = PadState::Covered;
        }
        measureRange(encoding);
    }

    [[nodiscard]] bool hasCoverage() const { return usable_ && covered_; }

    // Grows the band by one texel. Returns the number of texels filled; zero
    // means the image is fully covered and no further ring can add anything.
    std::size_t growRing(PaddingMode mode) {
        collectCandidates();
        for (const std::size_t at : candidates_) {
            const int x = static_cast<int>(at % static_cast<std::size_t>(image_.width));
            const int y = static_cast<int>(at / static_cast<std::size_t>(image_.width));
            write(x, y, fill(x, y, mode));
        }
        for (const std::size_t at : candidates_) {
            state_[at] = PadState::Covered;
        }
        frontier_ = candidates_;
        return candidates_.size();
    }

private:
    static std::size_t pixelCount(const Image& image) {
        return static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    }

    [[nodiscard]] std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) +
               static_cast<std::size_t>(x);
    }

    [[nodiscard]] bool isSource(int x, int y) const {
        return x >= 0 && y >= 0 && x < image_.width && y < image_.height &&
               state_[index(x, y)] == PadState::Covered;
    }

    [[nodiscard]] PadTexel read(int x, int y) const {
        PadTexel out{};
        for (std::size_t c = 0; c < channels_; ++c) {
            out[c] = image_.at(x, y, static_cast<int>(c));
        }
        return out;
    }

    void write(int x, int y, const PadTexel& value) {
        for (std::size_t c = 0; c < channels_; ++c) {
            image_.at(x, y, static_cast<int>(c)) = value[c];
        }
    }

    // The covered texels, in raster order, and the band an extrapolated value
    // is confined to, per channel. That band is the intersection of two limits.
    //
    // The first is the COMPOUNDING bound: the range the originally covered
    // texels span, widened by that range's own width on each side. A bound is
    // needed because a ring reads the ring before it, so on a map that is not
    // locally linear the continuation compounds. Clamping to the covered range
    // ALONE would be wrong in the opposite direction: a map whose values ramp
    // across the island reaches its own extreme AT the border, so that clamp
    // would flatten every continuation into exactly the repeated edge value
    // this stage exists to avoid. One range's width of slack leaves a genuine
    // continuation room to run and still bounds a noisy map to three times its
    // own range.
    //
    // The second is the map's OWN value range, taken from the encoding that was
    // recorded with it. The compounding bound is deliberately wider than the
    // map -- for an object-space position, whose encoding guarantees every texel
    // is inside [0,1], it is [-1,2] -- and a band outside a map's declared range
    // is not a continuation, it is a value the map says cannot occur. A consumer
    // decoding an object-space position as `min + v*(max-min)` would get a point
    // outside the box the bake recorded; an occlusion would exceed unity; a
    // thickness would go negative. Maps whose encoding guarantees nothing
    // (a position in model units, a signed displacement) leave this infinite and
    // are bounded by the compounding limit alone.
    void measureRange(const BakeEncoding& encoding) {
        PadTexel low;
        PadTexel high;
        low.fill(std::numeric_limits<float>::infinity());
        high.fill(-std::numeric_limits<float>::infinity());
        for (int y = 0; y < image_.height; ++y) {
            for (int x = 0; x < image_.width; ++x) {
                if (state_[index(x, y)] != PadState::Covered) {
                    continue;
                }
                covered_ = true;
                // The first ring's frontier, in RASTER order -- the order the
                // whole band is then built in, and the reason it does not
                // depend on the order the rasteriser happened to emit texels.
                frontier_.push_back(index(x, y));
                const PadTexel value = read(x, y);
                for (std::size_t c = 0; c < channels_; ++c) {
                    low[c] = std::fmin(low[c], value[c]);
                    high[c] = std::fmax(high[c], value[c]);
                }
            }
        }
        if (!covered_) {
            return;
        }
        for (std::size_t c = 0; c < channels_; ++c) {
            const float span = high[c] - low[c];
            min_[c] = std::fmax(low[c] - span, encoding.valueMin);
            max_[c] = std::fmin(high[c] + span, encoding.valueMax);
        }
    }

    // The uncovered 8-neighbours of the last ring's texels: exactly the texels
    // at Chebyshev distance one from what is covered now. Gathered in the
    // frontier's own order, which starts in raster order and stays there.
    void collectCandidates() {
        candidates_.clear();
        for (const std::size_t at : frontier_) {
            const int x = static_cast<int>(at % static_cast<std::size_t>(image_.width));
            const int y = static_cast<int>(at / static_cast<std::size_t>(image_.width));
            for (const PadOffset& d : kCompass) {
                const int nx = x + d.dx;
                const int ny = y + d.dy;
                if (nx < 0 || ny < 0 || nx >= image_.width || ny >= image_.height) {
                    continue;
                }
                const std::size_t nat = index(nx, ny);
                if (state_[nat] != PadState::Empty) {
                    continue;
                }
                state_[nat] = PadState::Queued;
                candidates_.push_back(nat);
            }
        }
    }

    PadTexel fill(int x, int y, PaddingMode mode) const {
        if (mode == PaddingMode::Nearest) {
            return nearest(x, y);
        }
        const PadTexel value = extrapolate(x, y);
        return mode == PaddingMode::ExtrapolateUnit ? unitDirection(value) : clamped(value);
    }

    // The nearest covered texel's value, copied verbatim, first hit in compass
    // order. A candidate always has one: it was collected as a neighbour of a
    // covered texel.
    [[nodiscard]] PadTexel nearest(int x, int y) const {
        for (const PadOffset& d : kCompass) {
            if (isSource(x + d.dx, y + d.dy)) {
                return read(x + d.dx, y + d.dy);
            }
        }
        return read(x, y);
    }

    // For each covered compass neighbour, the linear continuation `2*v1 - v2`
    // of the gradient its two samples along that direction define, averaged
    // over the directions that hit. A direction whose second sample is not
    // covered carries no gradient and its term collapses to v1, which is the
    // only information it has. Averaging rather than picking one direction
    // keeps the band from tilting toward whichever compass point is enumerated
    // first along a straight border, where three neighbours are covered.
    [[nodiscard]] PadTexel extrapolate(int x, int y) const {
        PadTexel sum{};
        int hits = 0;
        for (const PadOffset& d : kCompass) {
            if (!isSource(x + d.dx, y + d.dy)) {
                continue;
            }
            // `inner` / `beyond` rather than near/far: the latter are legacy
            // macros in <windows.h> and this file compiles on MSVC too.
            const PadTexel inner = read(x + d.dx, y + d.dy);
            const bool hasSecond = isSource(x + 2 * d.dx, y + 2 * d.dy);
            const PadTexel beyond = hasSecond ? read(x + 2 * d.dx, y + 2 * d.dy) : inner;
            for (std::size_t c = 0; c < channels_; ++c) {
                sum[c] += 2.0f * inner[c] - beyond[c];
            }
            ++hits;
        }
        if (hits == 0) {
            return read(x, y);
        }
        for (std::size_t c = 0; c < channels_; ++c) {
            sum[c] /= static_cast<float>(hits);
        }
        return sum;
    }

    [[nodiscard]] PadTexel clamped(PadTexel value) const {
        for (std::size_t c = 0; c < channels_; ++c) {
            value[c] = std::clamp(value[c], min_[c], max_[c]);
        }
        return value;
    }

    // An extrapolated direction is not a unit direction, and a shortened normal
    // darkens the padded band in every shader that does not renormalize. The
    // range clamp is deliberately NOT applied here: a re-encoded unit vector is
    // already inside [0,1] on every channel -- which is the whole range a
    // direction map's encoding guarantees -- and clamping after normalizing
    // would shorten it again.
    [[nodiscard]] static PadTexel unitDirection(const PadTexel& value) {
        const Vec3 decoded{value[0] * 2.0f - 1.0f, value[1] * 2.0f - 1.0f, value[2] * 2.0f - 1.0f};
        const float len = length(decoded);
        // A collapsed extrapolation has no direction left to preserve, so the
        // texel takes +Z: the neutral of both direction bases (a flat tangent
        // normal, and an up-facing object normal once the up axis is applied).
        const Vec3 unit = len > 1e-6f ? decoded / len : Vec3{0.0f, 0.0f, 1.0f};
        return PadTexel{unit.x * 0.5f + 0.5f, unit.y * 0.5f + 0.5f, unit.z * 0.5f + 0.5f, 0.0f};
    }

    Image& image_;
    std::size_t channels_;
    std::vector<PadState> state_;
    std::vector<std::size_t> frontier_;
    std::vector<std::size_t> candidates_;
    PadTexel min_{};
    PadTexel max_{};
    bool usable_ = false;
    bool covered_ = false;
};

}  // namespace

PaddingMode paddingModeFor(EncodingBasis basis) {
    switch (basis) {
        case EncodingBasis::IdColor:
            // NEAREST NEIGHBOUR, never extrapolated. An id map's texels are
            // exact keys; a value between two of them is a colour that resolves
            // to no id, and the zero-tolerance comparison the map exists for
            // then fails on the whole band.
            return PaddingMode::Nearest;
        case EncodingBasis::TangentNormal:
        case EncodingBasis::ObjectNormal:
            return PaddingMode::ExtrapolateUnit;
        case EncodingBasis::None:
        case EncodingBasis::ObjectBounds:
        case EncodingBasis::Distance:
            break;
    }
    // Everything else continues its gradient, bounded by the map's own declared
    // value range (BakeEncoding::valueMin/valueMax) as well as by the
    // compounding limit -- which is what keeps an object-space position inside
    // [0,1], an occlusion at or under unity, and a thickness non-negative.
    return PaddingMode::Extrapolate;
}

PadOutcome padBorders(Image& image, const std::vector<PadCoord>& covered,
                      const BakeEncoding& encoding, int radius, const CancelToken* cancel) {
    PadOutcome out;
    out.padding.radius = radius;
    if (radius <= 0 || image.pixels.empty() || covered.empty()) {
        return out;
    }
    BorderPadder padder(image, covered, encoding);
    if (!padder.hasCoverage()) {
        return out;
    }
    const PaddingMode mode = paddingModeFor(encoding.basis);
    for (int ring = 0; ring < radius; ++ring) {
        if (cancel != nullptr && cancel->isCancelled()) {
            out.cancelled = true;
            return out;
        }
        const std::size_t filled = padder.growRing(mode);
        if (filled == 0) {
            break;  // the image is fully covered; further rings cannot add one
        }
        out.padding.texelsFilled += filled;
    }
    if (out.padding.texelsFilled > 0) {
        out.padding.mode = mode;
    }
    return out;
}

}  // namespace cyber::bake::detail
