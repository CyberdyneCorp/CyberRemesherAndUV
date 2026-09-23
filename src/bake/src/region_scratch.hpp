#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cyber/bake/bake.hpp"

// Scratch storage for a regioned bake (surface-baking spec, "Regioned baking
// with a bounded working set").
//
// A regioned bake shades every texel ONCE, before anything that needs the whole
// image -- the padded band's clamp range, the relative density mean, a field
// curvature range -- is known. The shaded rows wait here, on disk, until those
// quantities have been measured over the whole image, and are then read back one
// assembly window at a time. Disk rather than memory because the whole point is
// an output larger than memory; it is the resource a host that "has the disk for
// a 16K map" has.
//
// One file per output image. A stored row is `width * channels` floats followed
// by `width` coverage bytes (1 = a texel the rasteriser covered). Native byte
// order: the file never leaves the process. Removed by the destructor, so every
// exit path -- completion, refusal, cancellation, failure -- removes it.
//
// SPARSE: a row no texel covers holds nothing but the map's neutral background
// (the shade pass writes only covered texels, and every later normalization
// touches only covered ones), so it is not stored at all -- reading it back
// synthesises the neutral row. A 16K map of a small chart therefore spills the
// chart's rows, not sixteen thousand of them. The row index is 8 bytes per row
// (128 KiB at 16384 rows) and lives in memory.
//
// INTERNAL to cyber_bake.
namespace cyber::bake::detail {

class RegionScratch {
public:
    RegionScratch(const RegionScratch&) = delete;
    RegionScratch& operator=(const RegionScratch&) = delete;
    RegionScratch(RegionScratch&&) = delete;
    RegionScratch& operator=(RegionScratch&&) = delete;
    ~RegionScratch();

    // Creates the file in `directory` (the system temporary directory when
    // empty). Null, with `error` set, when it cannot be created.
    // `neutral` is the value an uncovered texel holds, per channel.
    [[nodiscard]] static std::unique_ptr<RegionScratch> create(const std::string& directory,
                                                               int width, int height, int channels,
                                                               const std::array<float, 3>& neutral,
                                                               std::string& error);

    // Writes `rows.height` rows starting at output row `rowBegin`. `coverage`
    // holds `rows.width * rows.height` bytes.
    [[nodiscard]] bool write(int rowBegin, const Image& rows,
                             const std::vector<std::uint8_t>& coverage);

    // Reads rows [rowBegin, rowBegin + count) into `rows` (resized to fit) and
    // their coverage into `coverage`.
    [[nodiscard]] bool read(int rowBegin, int count, Image& rows,
                            std::vector<std::uint8_t>& coverage);

    // Whether any row of [rowBegin, rowEnd) is stored, i.e. holds a covered
    // texel. A range that stores none is neutral background throughout.
    [[nodiscard]] bool stores(int rowBegin, int rowEnd) const;

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] int channels() const { return channels_; }

private:
    RegionScratch(std::filesystem::path path, int width, int height, int channels,
                  const std::array<float, 3>& neutral);

    [[nodiscard]] bool writeRow(int row, const float* pixels, const std::uint8_t* coverage);
    [[nodiscard]] bool readRow(int row, float* pixels, std::uint8_t* coverage);

    static constexpr std::int64_t kNotStored = -1;

    std::filesystem::path path_;
    std::fstream file_;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 0;
    std::size_t pixelBytes_ = 0;       // one row's floats
    std::vector<float> neutralRow_;    // one row of the map's neutral background
    std::vector<std::int64_t> slots_;  // byte offset of each stored row, or kNotStored
    std::int64_t end_ = 0;             // where the next new row is appended
};

}  // namespace cyber::bake::detail
