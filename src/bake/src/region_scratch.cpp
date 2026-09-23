#include "region_scratch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>
#include <system_error>
#include <utility>

namespace cyber::bake::detail {

namespace {

// A name no other bake is likely to pick: 64 random bits. The file is created
// with truncation and removed on destruction; it holds shaded texels, nothing a
// neighbour process could use.
std::string scratchName() {
    std::random_device device;
    const auto high = static_cast<std::uint64_t>(device());
    const auto low = static_cast<std::uint64_t>(device());
    const std::uint64_t bits = (high << 32) ^ low;
    std::array<char, 17> hex{};
    std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(bits));
    return std::string("cyber-bake-region-") + hex.data() + ".scratch";
}

}  // namespace

RegionScratch::RegionScratch(std::filesystem::path path, int width, int height, int channels,
                             const std::array<float, 3>& neutral)
    : path_(std::move(path)),
      width_(width),
      height_(height),
      channels_(channels),
      pixelBytes_(static_cast<std::size_t>(width) * static_cast<std::size_t>(channels) *
                  sizeof(float)),
      slots_(static_cast<std::size_t>(height), kNotStored) {
    const auto ch = static_cast<std::size_t>(channels);
    neutralRow_.resize(static_cast<std::size_t>(width) * ch);
    for (std::size_t i = 0; i < neutralRow_.size(); ++i) {
        neutralRow_[i] = neutral[i % ch];
    }
}

RegionScratch::~RegionScratch() {
    file_.close();
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
}

std::unique_ptr<RegionScratch> RegionScratch::create(const std::string& directory, int width,
                                                     int height, int channels,
                                                     const std::array<float, 3>& neutral,
                                                     std::string& error) {
    std::error_code ec;
    std::filesystem::path base = directory.empty() ? std::filesystem::temp_directory_path(ec)
                                                   : std::filesystem::path(directory);
    if (ec) {
        error = "no temporary directory for the regioned bake's scratch file: " + ec.message();
        return nullptr;
    }
    if (base.empty()) {
        base = ".";
    }
    std::unique_ptr<RegionScratch> scratch(
        new RegionScratch(base / scratchName(), width, height, channels, neutral));
    scratch->file_.open(scratch->path_,
                        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!scratch->file_) {
        error = "cannot create the regioned bake's scratch file in '" + base.string() + "'";
        return nullptr;
    }
    return std::unique_ptr<RegionScratch>(std::move(scratch));
}

bool RegionScratch::writeRow(int row, const float* pixels, const std::uint8_t* coverage) {
    const auto w = static_cast<std::size_t>(width_);
    std::int64_t& slot = slots_[static_cast<std::size_t>(row)];
    if (slot == kNotStored) {
        if (std::find(coverage, coverage + w, std::uint8_t{1}) == coverage + w) {
            return true;  // neutral background only: synthesised on read
        }
        slot = end_;
        end_ += static_cast<std::int64_t>(pixelBytes_ + w);
    }
    file_.seekp(static_cast<std::streamoff>(slot));
    file_.write(reinterpret_cast<const char*>(pixels), static_cast<std::streamsize>(pixelBytes_));
    file_.write(reinterpret_cast<const char*>(coverage), static_cast<std::streamsize>(w));
    return static_cast<bool>(file_);
}

bool RegionScratch::readRow(int row, float* pixels, std::uint8_t* coverage) {
    const auto w = static_cast<std::size_t>(width_);
    const std::int64_t slot = slots_[static_cast<std::size_t>(row)];
    if (slot == kNotStored) {
        std::copy(neutralRow_.begin(), neutralRow_.end(), pixels);
        std::fill(coverage, coverage + w, std::uint8_t{0});
        return true;
    }
    file_.seekg(static_cast<std::streamoff>(slot));
    file_.read(reinterpret_cast<char*>(pixels), static_cast<std::streamsize>(pixelBytes_));
    file_.read(reinterpret_cast<char*>(coverage), static_cast<std::streamsize>(w));
    return static_cast<bool>(file_);
}

bool RegionScratch::stores(int rowBegin, int rowEnd) const {
    return std::any_of(slots_.begin() + rowBegin, slots_.begin() + rowEnd,
                       [](std::int64_t slot) { return slot != kNotStored; });
}

bool RegionScratch::write(int rowBegin, const Image& rows,
                          const std::vector<std::uint8_t>& coverage) {
    const auto w = static_cast<std::size_t>(width_);
    const auto floatsPerRow = w * static_cast<std::size_t>(channels_);
    for (int y = 0; y < rows.height; ++y) {
        const auto row = static_cast<std::size_t>(y);
        if (!writeRow(rowBegin + y, rows.pixels.data() + row * floatsPerRow,
                      coverage.data() + row * w)) {
            return false;
        }
    }
    // Flushed here so a full disk is reported at the region that hit it rather
    // than at a later read that would find stale bytes.
    file_.flush();
    return static_cast<bool>(file_);
}

bool RegionScratch::read(int rowBegin, int count, Image& rows,
                         std::vector<std::uint8_t>& coverage) {
    const auto w = static_cast<std::size_t>(width_);
    const auto floatsPerRow = w * static_cast<std::size_t>(channels_);
    rows.width = width_;
    rows.height = count;
    rows.channels = channels_;
    rows.pixels.resize(floatsPerRow * static_cast<std::size_t>(count));
    coverage.resize(w * static_cast<std::size_t>(count));
    for (int y = 0; y < count; ++y) {
        const auto row = static_cast<std::size_t>(y);
        if (!readRow(rowBegin + y, rows.pixels.data() + row * floatsPerRow,
                     coverage.data() + row * w)) {
            return false;
        }
    }
    return true;
}

}  // namespace cyber::bake::detail
