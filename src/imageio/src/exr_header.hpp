#pragma once

// The OpenEXR header and channel layout, shared by the one-shot writer
// (writeExr) and the band writer (ImageStreamWriter), so the two cannot
// disagree about a single header byte. Internal to cyber_imageio.

#include <cstdint>
#include <string>
#include <vector>

#include "detail.hpp"

namespace cyber::imageio::detail {

// One output channel: EXR channel name plus which source component (R=0, G=1,
// B=2, A=3) it draws from. Channels must be listed in alphabetical order.
struct ExrChannel {
    std::string name;
    int source;
};

// The alphabetically ordered channel set for a given channel count, or an empty
// vector for unsupported counts.
inline std::vector<ExrChannel> exrChannelLayout(int channels) {
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

inline void exrWriteAttr(Bytes& out, const std::string& name, const std::string& type,
                         const Bytes& value) {
    appendCString(out, name);
    appendCString(out, type);
    appendU32LE(out, static_cast<std::uint32_t>(value.size()));
    appendBytes(out, value);
}

inline Bytes exrBox2i(std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax) {
    Bytes v;
    appendI32LE(v, xMin);
    appendI32LE(v, yMin);
    appendI32LE(v, xMax);
    appendI32LE(v, yMax);
    return v;
}

// Magic, version and every header attribute, through the end-of-header byte.
// The scanline offset table follows it.
inline Bytes exrHeader(int width, int height, const std::vector<ExrChannel>& layout) {
    Bytes out;
    appendU32LE(out, 0x01312f76u);  // magic
    appendU32LE(out, 2u);           // version 2, no special flags

    Bytes channelList;
    for (const ExrChannel& ch : layout) {
        appendCString(channelList, ch.name);
        appendU32LE(channelList, 2);  // pixel type: FLOAT
        channelList.push_back(0);     // pLinear
        channelList.push_back(0);     // reserved
        channelList.push_back(0);
        channelList.push_back(0);
        appendU32LE(channelList, 1);  // xSampling
        appendU32LE(channelList, 1);  // ySampling
    }
    channelList.push_back(0);  // end of channel list
    exrWriteAttr(out, "channels", "chlist", channelList);
    exrWriteAttr(out, "compression", "compression", Bytes{0});  // NO_COMPRESSION
    exrWriteAttr(out, "dataWindow", "box2i",
                 exrBox2i(0, 0, static_cast<std::int32_t>(width - 1),
                          static_cast<std::int32_t>(height - 1)));
    exrWriteAttr(out, "displayWindow", "box2i",
                 exrBox2i(0, 0, static_cast<std::int32_t>(width - 1),
                          static_cast<std::int32_t>(height - 1)));
    exrWriteAttr(out, "lineOrder", "lineOrder", Bytes{0});  // INCREASING_Y
    {
        Bytes v;
        appendFloatLE(v, 1.0f);
        exrWriteAttr(out, "pixelAspectRatio", "float", v);
    }
    {
        Bytes v;
        appendFloatLE(v, 0.0f);
        appendFloatLE(v, 0.0f);
        exrWriteAttr(out, "screenWindowCenter", "v2f", v);
    }
    {
        Bytes v;
        appendFloatLE(v, 1.0f);
        exrWriteAttr(out, "screenWindowWidth", "float", v);
    }
    out.push_back(0);  // end of header
    return out;
}

}  // namespace cyber::imageio::detail
