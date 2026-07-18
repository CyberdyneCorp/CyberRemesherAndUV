#include "cyber/imageio/exr.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "detail.hpp"

namespace cyber::imageio {

namespace {

using detail::Bytes;

// One output channel: EXR channel name plus which source component (R=0, G=1,
// B=2, A=3) it draws from. Channels must be listed in alphabetical order.
struct Channel {
    std::string name;
    int source;
};

// Returns the alphabetically ordered channel set for a given channel count, or
// an empty vector for unsupported counts.
std::vector<Channel> channelLayout(int channels) {
    switch (channels) {
        case 1:
            return {{"Y", 0}};
        case 3:
            return {{"B", 2}, {"G", 1}, {"R", 0}};
        case 4:
            return {{"A", 3}, {"B", 2}, {"G", 1}, {"R", 0}};
        default:
            return {};
    }
}

void writeAttr(Bytes& out, const std::string& name, const std::string& type, const Bytes& value) {
    detail::appendCString(out, name);
    detail::appendCString(out, type);
    detail::appendU32LE(out, static_cast<std::uint32_t>(value.size()));
    detail::appendBytes(out, value);
}

Bytes channelListAttr(const std::vector<Channel>& channels) {
    Bytes v;
    for (const Channel& ch : channels) {
        detail::appendCString(v, ch.name);
        detail::appendU32LE(v, 2);  // pixel type: FLOAT
        v.push_back(0);             // pLinear
        v.push_back(0);             // reserved
        v.push_back(0);
        v.push_back(0);
        detail::appendU32LE(v, 1);  // xSampling
        detail::appendU32LE(v, 1);  // ySampling
    }
    v.push_back(0);  // end of channel list
    return v;
}

Bytes box2iAttr(std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax) {
    Bytes v;
    detail::appendI32LE(v, xMin);
    detail::appendI32LE(v, yMin);
    detail::appendI32LE(v, xMax);
    detail::appendI32LE(v, yMax);
    return v;
}

}  // namespace

bool writeExr(const std::string& path, int width, int height, int channels, const float* pixels) {
    const std::vector<Channel> layout = channelLayout(channels);
    if (width <= 0 || height <= 0 || layout.empty() || pixels == nullptr) {
        return false;
    }
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    const std::size_t ch = static_cast<std::size_t>(channels);

    Bytes out;
    detail::appendU32LE(out, 0x01312f76u);  // magic
    detail::appendU32LE(out, 2u);           // version 2, no special flags

    // Header attributes.
    writeAttr(out, "channels", "chlist", channelListAttr(layout));
    writeAttr(out, "compression", "compression", Bytes{0});  // NO_COMPRESSION
    writeAttr(out, "dataWindow", "box2i",
              box2iAttr(0, 0, static_cast<std::int32_t>(width - 1),
                        static_cast<std::int32_t>(height - 1)));
    writeAttr(out, "displayWindow", "box2i",
              box2iAttr(0, 0, static_cast<std::int32_t>(width - 1),
                        static_cast<std::int32_t>(height - 1)));
    writeAttr(out, "lineOrder", "lineOrder", Bytes{0});  // INCREASING_Y
    {
        Bytes v;
        detail::appendFloatLE(v, 1.0f);
        writeAttr(out, "pixelAspectRatio", "float", v);
    }
    {
        Bytes v;
        detail::appendFloatLE(v, 0.0f);
        detail::appendFloatLE(v, 0.0f);
        writeAttr(out, "screenWindowCenter", "v2f", v);
    }
    {
        Bytes v;
        detail::appendFloatLE(v, 1.0f);
        writeAttr(out, "screenWindowWidth", "float", v);
    }
    out.push_back(0);  // end of header

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
        for (const Channel& c : layout) {
            const std::size_t src = static_cast<std::size_t>(c.source);
            for (std::size_t x = 0; x < w; ++x) {
                detail::appendFloatLE(out, row[x * ch + src]);
            }
        }
    }

    return detail::writeFile(path, out);
}

}  // namespace cyber::imageio
