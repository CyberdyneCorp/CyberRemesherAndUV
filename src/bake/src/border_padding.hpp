#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/progress.hpp"

// UV border padding (surface-baking spec, "Bake output padding across UV island
// borders"), as its own translation unit.
//
// It lives beside bake.cpp rather than inside it because the stage is a pure
// image operation -- it needs no mesh, no cage, no rays, only the finished image
// and the texels the rasteriser covered -- and because the guarantees the spec
// makes about it (a band that never leaves the map's own value range, a band
// that does not depend on the order texels are visited in, a band that honours
// cancellation) can then be asserted directly on an image the test authors,
// instead of inferred from a bake.
//
// This header is INTERNAL to cyber_bake: it is not installed and not part of the
// public API. tests/bake/test_border_padding.cpp includes it through the source
// directory, the same way tests/cli/test_parse_number.cpp includes the CLI's
// header-only number parser.
namespace cyber::bake::detail {

// One covered texel's image coordinates. The padding stage wants nothing else
// from a Texel, and taking only this keeps the stage independent of how the
// coverage was produced (rasteriser, field path, or a test).
struct PadCoord {
    int x = 0;
    int y = 0;
};

// The fill rule a map's channel semantics demand, read off the recorded encoding
// basis rather than off the map's name: a map type added later gets the right
// rule as long as it records an honest basis.
[[nodiscard]] PaddingMode paddingModeFor(EncodingBasis basis);

// What padBorders() did. `cancelled` is true when the token tripped part-way
// through the band; the image then holds however many rings had been written,
// and the caller discards it exactly as it discards a cancelled shade.
struct PadOutcome {
    BakePadding padding;
    bool cancelled = false;
};

// The band an extrapolated texel is confined to, per channel: the COMPOUNDING
// limit (the covered values' range widened by its own width on each side)
// intersected with the map's declared value range. See BorderPadder for why.
//
// A value rather than a detail of the padder because it is a statistic of the
// WHOLE output image: a regioned bake pads one window at a time, and a range
// measured per window would clamp the band differently in every region -- a
// seam that appears only in the padding of a bake whose shading was seamless.
// The regioned path measures it once over the whole image and hands it in.
struct PadRange {
    std::array<float, 4> min{};
    std::array<float, 4> max{};
    bool covered = false;  // whether any covered texel contributed
};

// Accumulates a PadRange one covered texel at a time. The ordinary padder and
// the regioned path both measure through this, so the arithmetic -- and
// therefore the band -- is the same whichever path measured it.
class PadRangeAccumulator {
public:
    explicit PadRangeAccumulator(int channels);

    // `texel` holds `channels` floats.
    void add(const float* texel);

    [[nodiscard]] PadRange finish(const BakeEncoding& encoding) const;

private:
    std::size_t channels_;
    std::array<float, 4> low_{};
    std::array<float, 4> high_{};
    bool covered_ = false;
};

// Rows [begin, end) of the image whose filled texels a padding pass COUNTS in
// its report. The whole image for an ordinary bake; a region's own rows for a
// window that also holds its halo, so every texel is counted by exactly one
// region.
struct PadRows {
    int begin = 0;
    int end = 0;
};

// Pads `image` outward from `covered` and reports what it did. `encoding`
// supplies both the fill rule (through its basis) and the value range the band
// is confined to (through valueMin/valueMax). A radius of zero, an empty image
// or empty coverage is a no-op that still reports the radius.
[[nodiscard]] PadOutcome padBorders(Image& image, const std::vector<PadCoord>& covered,
                                    const BakeEncoding& encoding, int radius,
                                    const CancelToken* cancel);

// The same, against a range measured elsewhere (over the whole output, for a
// window of it) and counting only the texels filled in `counted`.
[[nodiscard]] PadOutcome padBorders(Image& image, const std::vector<PadCoord>& covered,
                                    const BakeEncoding& encoding, int radius,
                                    const CancelToken* cancel, const PadRange& range,
                                    PadRows counted);

}  // namespace cyber::bake::detail
