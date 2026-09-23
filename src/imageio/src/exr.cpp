#include "cyber/imageio/exr.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "detail.hpp"
#include "exr_header.hpp"

namespace cyber::imageio {

bool writeExr(const std::string& path, int width, int height, int channels, const float* pixels) {
    const std::vector<detail::ExrChannel> layout = detail::exrChannelLayout(channels);
    if (width <= 0 || height <= 0 || layout.empty() || pixels == nullptr) {
        return false;
    }
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    const std::size_t ch = static_cast<std::size_t>(channels);

    detail::Bytes out = detail::exrHeader(width, height, layout);

    // Scanline offset table: one entry per scanline (one block each, since the
    // data is uncompressed). Each block is [int32 y][int32 dataSize][pixels].
    const std::size_t pixelBytes = ch * w * 4;
    const std::size_t blockStride = 8 + pixelBytes;
    const std::size_t tableStart = out.size();
    const std::size_t firstBlock = tableStart + h * 8;
    for (std::size_t y = 0; y < h; ++y) {
        detail::appendU64LE(out, static_cast<std::uint64_t>(firstBlock + y * blockStride));
    }

    // Scanline blocks.
    for (std::size_t y = 0; y < h; ++y) {
        detail::appendI32LE(out, static_cast<std::int32_t>(y));
        detail::appendU32LE(out, static_cast<std::uint32_t>(pixelBytes));
        const float* row = pixels + y * w * ch;
        for (const detail::ExrChannel& c : layout) {
            const std::size_t src = static_cast<std::size_t>(c.source);
            for (std::size_t x = 0; x < w; ++x) {
                detail::appendFloatLE(out, row[x * ch + src]);
            }
        }
    }

    return detail::writeFile(path, out);
}

}  // namespace cyber::imageio
