#include "cyber/imageio/load.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>

#include "detail.hpp"

namespace cyber::imageio {

namespace {

using detail::Bytes;

std::uint32_t readU32BE(const Bytes& b, std::size_t o) {
    return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16) |
           (static_cast<std::uint32_t>(b[o + 2]) << 8) | static_cast<std::uint32_t>(b[o + 3]);
}

std::uint16_t readU16LE(const Bytes& b, std::size_t o) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(b[o]) |
                                      (static_cast<std::uint32_t>(b[o + 1]) << 8));
}

Bytes readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return Bytes{};
    }
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The 8-byte PNG signature this writer emits.
bool hasPngSignature(const Bytes& b) {
    static const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (b.size() < 8) {
        return false;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (b[i] != sig[i]) {
            return false;
        }
    }
    return true;
}

// Parsed IHDR fields for the subset of PNG we accept.
struct Header {
    int width = 0;
    int height = 0;
    int channels = 0;
};

// Validates a 13-byte IHDR payload and extracts dimensions/channel count.
// Accepts only 8-bit RGB (colour type 2) / RGBA (colour type 6), no
// interlacing, standard filter method.
std::optional<Header> parseIhdr(const Bytes& data) {
    if (data.size() != 13) {
        return std::nullopt;
    }
    const std::uint32_t w = readU32BE(data, 0);
    const std::uint32_t h = readU32BE(data, 4);
    const std::uint8_t bitDepth = data[8];
    const std::uint8_t colourType = data[9];
    const std::uint8_t compression = data[10];
    const std::uint8_t filterMethod = data[11];
    const std::uint8_t interlace = data[12];
    constexpr std::uint32_t kMaxDim = 0x7fffffffu;
    if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim) {
        return std::nullopt;
    }
    if (bitDepth != 8 || compression != 0 || filterMethod != 0 || interlace != 0) {
        return std::nullopt;
    }
    if (colourType != 2 && colourType != 6) {
        return std::nullopt;
    }
    Header hdr;
    hdr.width = static_cast<int>(w);
    hdr.height = static_cast<int>(h);
    hdr.channels = (colourType == 6) ? 4 : 3;
    return hdr;
}

// Walks the chunk stream after the signature, validating each chunk's CRC-32
// over type+data. Fills the IHDR header and concatenates every IDAT payload.
// Returns false on any truncation, CRC mismatch, or malformed IHDR.
bool collectChunks(const Bytes& b, Header& header, Bytes& idat) {
    std::size_t pos = 8;
    bool sawIhdr = false;
    while (pos + 12 <= b.size()) {
        const std::size_t len = readU32BE(b, pos);
        const std::size_t dataStart = pos + 8;
        if (dataStart + len + 4 > b.size()) {
            return false;  // chunk claims more bytes than the file holds
        }
        const std::uint32_t storedCrc = readU32BE(b, dataStart + len);
        const std::uint32_t computed = detail::crc32(b.data() + pos + 4, 4 + len);
        if (storedCrc != computed) {
            return false;
        }
        const std::uint8_t* type = b.data() + pos + 4;
        if (std::equal(type, type + 4, "IHDR")) {
            const auto hdr = parseIhdr(Bytes(b.begin() + static_cast<std::ptrdiff_t>(dataStart),
                                             b.begin() + static_cast<std::ptrdiff_t>(dataStart + len)));
            if (!hdr) {
                return false;
            }
            header = *hdr;
            sawIhdr = true;
        } else if (std::equal(type, type + 4, "IDAT")) {
            idat.insert(idat.end(), b.begin() + static_cast<std::ptrdiff_t>(dataStart),
                        b.begin() + static_cast<std::ptrdiff_t>(dataStart + len));
        } else if (std::equal(type, type + 4, "IEND")) {
            return sawIhdr;
        }
        pos = dataStart + len + 4;
    }
    return false;  // ran out of bytes before IEND
}

// Inflates a zlib stream restricted to STORED (BTYPE=00) DEFLATE blocks, as
// emitted by zlibStored() in png.cpp. Validates the zlib header and the
// trailing Adler-32. Returns std::nullopt for any other block type or a bad
// checksum.
std::optional<Bytes> inflateStored(const Bytes& z) {
    if (z.size() < 6) {  // 2 header + at least one empty final block-less trailer + 4 adler
        return std::nullopt;
    }
    const unsigned cmf = z[0];
    const unsigned flg = z[1];
    if ((cmf & 0x0fu) != 8u) {  // compression method must be deflate
        return std::nullopt;
    }
    if (((cmf << 8) | flg) % 31u != 0u) {  // header checksum
        return std::nullopt;
    }
    if ((flg & 0x20u) != 0u) {  // preset dictionary not supported
        return std::nullopt;
    }
    Bytes out;
    std::size_t pos = 2;
    bool finalBlock = false;
    while (!finalBlock) {
        if (pos >= z.size()) {
            return std::nullopt;
        }
        const std::uint8_t blockHeader = z[pos++];
        finalBlock = (blockHeader & 0x01u) != 0u;
        const unsigned btype = (blockHeader >> 1) & 0x03u;
        if (btype != 0u) {  // only STORED blocks are in scope
            return std::nullopt;
        }
        if (pos + 4 > z.size()) {
            return std::nullopt;
        }
        const std::uint16_t len = readU16LE(z, pos);
        const std::uint16_t nlen = readU16LE(z, pos + 2);
        if (len != static_cast<std::uint16_t>(~nlen)) {
            return std::nullopt;
        }
        pos += 4;
        if (pos + len > z.size()) {
            return std::nullopt;
        }
        out.insert(out.end(), z.begin() + static_cast<std::ptrdiff_t>(pos),
                   z.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    }
    if (pos + 4 > z.size()) {
        return std::nullopt;
    }
    if (readU32BE(z, pos) != detail::adler32(out.data(), out.size())) {
        return std::nullopt;
    }
    return out;
}

// Strips the per-scanline filter byte (only filter type 0, None, is accepted)
// and converts the 8-bit samples to normalised floats in [0,1].
std::optional<std::vector<float>> unfilterToFloat(const Bytes& raw, const Header& hdr) {
    const std::size_t w = static_cast<std::size_t>(hdr.width);
    const std::size_t h = static_cast<std::size_t>(hdr.height);
    const std::size_t ch = static_cast<std::size_t>(hdr.channels);
    const std::size_t rowBytes = w * ch;
    if (raw.size() != (rowBytes + 1) * h) {
        return std::nullopt;
    }
    std::vector<float> pixels(rowBytes * h);
    for (std::size_t y = 0; y < h; ++y) {
        const std::size_t rowStart = y * (rowBytes + 1);
        if (raw[rowStart] != 0) {  // only the None filter is supported
            return std::nullopt;
        }
        for (std::size_t x = 0; x < rowBytes; ++x) {
            pixels[y * rowBytes + x] = static_cast<float>(raw[rowStart + 1 + x]) / 255.0f;
        }
    }
    return pixels;
}


}  // namespace

std::optional<LoadedImage> loadPng(const std::string& path) {
    const Bytes bytes = readFileBytes(path);
    if (!hasPngSignature(bytes)) {
        return std::nullopt;
    }
    Header header;
    Bytes idat;
    if (!collectChunks(bytes, header, idat)) {
        return std::nullopt;
    }
    const auto raw = inflateStored(idat);
    if (!raw) {
        return std::nullopt;
    }
    auto pixels = unfilterToFloat(*raw, header);
    if (!pixels) {
        return std::nullopt;
    }
    LoadedImage image;
    image.width = header.width;
    image.height = header.height;
    image.channels = header.channels;
    image.pixels = std::move(*pixels);
    return image;
}


}  // namespace cyber::imageio
