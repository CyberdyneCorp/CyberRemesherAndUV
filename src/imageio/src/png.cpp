#include "cyber/imageio/png.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "detail.hpp"

namespace cyber::imageio {

namespace {

using detail::Bytes;

// Appends one PNG chunk: length (BE) + type + data + CRC-32 over type+data.
void writeChunk(Bytes& out, const char type[4], const Bytes& data) {
    detail::appendU32BE(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t crcStart = out.size();
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(type[i]));
    }
    detail::appendBytes(out, data);
    const std::uint32_t crc = detail::crc32(out.data() + crcStart, out.size() - crcStart);
    detail::appendU32BE(out, crc);
}

// Wraps the filtered scanlines in a zlib stream built from STORED deflate
// blocks (BTYPE=00), followed by the Adler-32 of the raw data.
Bytes zlibStored(const Bytes& raw) {
    Bytes out;
    out.push_back(0x78);  // CMF: 32K window, deflate
    out.push_back(0x01);  // FLG: chosen so (CMF*256+FLG) % 31 == 0, no dict
    constexpr std::size_t kMaxBlock = 65535;
    std::size_t pos = 0;
    do {
        const std::size_t block = std::min(kMaxBlock, raw.size() - pos);
        const bool final = (pos + block) == raw.size();
        out.push_back(final ? 0x01 : 0x00);
        const std::uint16_t len = static_cast<std::uint16_t>(block);
        detail::appendU16LE(out, len);
        detail::appendU16LE(out, static_cast<std::uint16_t>(~len & 0xffffu));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                   raw.begin() + static_cast<std::ptrdiff_t>(pos + block));
        pos += block;
    } while (pos < raw.size());
    detail::appendU32BE(out, detail::adler32(raw.data(), raw.size()));
    return out;
}

}  // namespace

bool writePng(const std::string& path, int width, int height, int channels,
              const std::uint8_t* rgba) {
    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4) || rgba == nullptr) {
        return false;
    }
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    const std::size_t ch = static_cast<std::size_t>(channels);
    const std::size_t rowBytes = w * ch;

    // Filtered scanlines: each row is prefixed with a filter-type byte (0 = None).
    Bytes raw;
    raw.reserve((rowBytes + 1) * h);
    for (std::size_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const std::uint8_t* row = rgba + y * rowBytes;
        raw.insert(raw.end(), row, row + rowBytes);
    }

    Bytes out;
    const std::uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), kSignature, kSignature + 8);

    Bytes ihdr;
    detail::appendU32BE(ihdr, static_cast<std::uint32_t>(width));
    detail::appendU32BE(ihdr, static_cast<std::uint32_t>(height));
    ihdr.push_back(8);                                              // bit depth
    ihdr.push_back(static_cast<std::uint8_t>(channels == 4 ? 6 : 2));  // color type
    ihdr.push_back(0);                                              // compression
    ihdr.push_back(0);                                              // filter method
    ihdr.push_back(0);                                              // interlace
    writeChunk(out, "IHDR", ihdr);

    writeChunk(out, "IDAT", zlibStored(raw));
    writeChunk(out, "IEND", Bytes{});

    return detail::writeFile(path, out);
}

bool writePngTonemapped(const std::string& path, int width, int height, int channels,
                        const float* pixels) {
    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4) || pixels == nullptr) {
        return false;
    }
    const std::size_t count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
        static_cast<std::size_t>(channels);
    detail::Bytes bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float clamped = std::min(1.0f, std::max(0.0f, pixels[i]));
        bytes[i] = static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
    }
    return writePng(path, width, height, channels, bytes.data());
}

}  // namespace cyber::imageio
