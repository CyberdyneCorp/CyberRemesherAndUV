#include <cstdlib>
#include <cyber/imageio/load.hpp>

#include "harness.hpp"

namespace cyber::fuzz {

int png(const std::uint8_t* data, std::size_t size) {
    TempInput input(data, size, ".png");
    if (!input.ok()) {
        return 0;
    }
    // loadPng must return std::nullopt for anything malformed, never read out of
    // bounds and never commit memory a truncated stream has no bytes for. The
    // return value is deliberately unused: the interesting outcomes are the ones
    // a sanitizer reports.
    const auto image = imageio::loadPng(input.path().string());
    if (image.has_value()) {
        // A decode that succeeds must describe its own buffer: a bogus
        // width/height/channel triple is the shape every out-of-bounds read in a
        // consumer starts from, so treat a mismatch as a finding in its own right.
        if (image->width < 0 || image->height < 0 || image->channels < 0) {
            std::abort();
        }
        const std::size_t expected = static_cast<std::size_t>(image->width) *
                                     static_cast<std::size_t>(image->height) *
                                     static_cast<std::size_t>(image->channels);
        if (image->pixels.size() != expected) {
            std::abort();
        }
    }
    return 0;
}

}  // namespace cyber::fuzz
