#include "cyber/imageio/load.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

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

// The 8-byte PNG signature.
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

// ---------------------------------------------------------------------------
// DEFLATE / zlib inflate (RFC 1950 / 1951)
// ---------------------------------------------------------------------------

// LSB-first bit reader over a byte buffer. Huffman codes are consumed bit by
// bit; multi-bit integer fields (block header, extra bits) are little-endian.
struct BitReader {
    const Bytes& data;
    std::size_t bytePos = 0;
    std::uint32_t bitBuf = 0;
    unsigned bitCount = 0;
    bool error = false;

    explicit BitReader(const Bytes& d, std::size_t start) : data(d), bytePos(start) {}

    unsigned getBit() {
        if (bitCount == 0) {
            if (bytePos >= data.size()) {
                error = true;
                return 0;
            }
            bitBuf = data[bytePos++];
            bitCount = 8;
        }
        const unsigned b = bitBuf & 1u;
        bitBuf >>= 1;
        --bitCount;
        return b;
    }

    unsigned getBits(unsigned n) {
        unsigned v = 0;
        for (unsigned i = 0; i < n; ++i) {
            v |= getBit() << i;
        }
        return v;
    }

    // Discard any buffered bits so bytePos sits on the next whole byte.
    void alignToByte() { bitCount = 0; }
};

// Canonical Huffman decode table built from per-symbol code lengths (puff.c
// style: counts of codes per length plus symbols sorted by (length, symbol)).
struct Huffman {
    std::array<int, 16> counts{};
    std::vector<int> symbols;

    bool build(const std::vector<int>& lengths) {
        counts.fill(0);
        for (const int len : lengths) {
            if (len < 0 || len > 15) {
                return false;
            }
            ++counts[static_cast<std::size_t>(len)];
        }
        counts[0] = 0;  // zero-length codes are absent, not real symbols
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= counts[static_cast<std::size_t>(len)];
            if (left < 0) {
                return false;  // over-subscribed code set
            }
        }
        std::array<int, 16> offsets{};
        for (int len = 1; len < 15; ++len) {
            offsets[static_cast<std::size_t>(len) + 1] =
                offsets[static_cast<std::size_t>(len)] + counts[static_cast<std::size_t>(len)];
        }
        symbols.assign(lengths.size(), 0);
        for (int sym = 0; sym < static_cast<int>(lengths.size()); ++sym) {
            const int len = lengths[static_cast<std::size_t>(sym)];
            if (len != 0) {
                const std::size_t slot =
                    static_cast<std::size_t>(offsets[static_cast<std::size_t>(len)]++);
                symbols[slot] = sym;
            }
        }
        return true;
    }

    int decode(BitReader& br) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(br.getBit());
            const int count = counts[static_cast<std::size_t>(len)];
            if (code - first < count) {
                return symbols[static_cast<std::size_t>(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;  // invalid code
    }
};

// RFC 1951 length codes 257..285: base value and count of extra bits.
constexpr std::array<int, 29> kLengthBase = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                             15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
// Distance codes 0..29.
constexpr std::array<int, 30> kDistBase = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

Huffman fixedLiteralTable() {
    std::vector<int> lengths(288);
    for (int i = 0; i <= 143; ++i) lengths[static_cast<std::size_t>(i)] = 8;
    for (int i = 144; i <= 255; ++i) lengths[static_cast<std::size_t>(i)] = 9;
    for (int i = 256; i <= 279; ++i) lengths[static_cast<std::size_t>(i)] = 7;
    for (int i = 280; i <= 287; ++i) lengths[static_cast<std::size_t>(i)] = 8;
    Huffman h;
    h.build(lengths);
    return h;
}

Huffman fixedDistanceTable() {
    Huffman h;
    h.build(std::vector<int>(30, 5));
    return h;
}

// Copies a stored (BTYPE=00) block into `out`. The block is byte-aligned.
bool inflateStored(BitReader& br, Bytes& out) {
    br.alignToByte();
    if (br.bytePos + 4 > br.data.size()) {
        return false;
    }
    const std::uint16_t len = readU16LE(br.data, br.bytePos);
    const std::uint16_t nlen = readU16LE(br.data, br.bytePos + 2);
    br.bytePos += 4;
    if (len != static_cast<std::uint16_t>(~nlen)) {
        return false;
    }
    if (br.bytePos + len > br.data.size()) {
        return false;
    }
    out.insert(out.end(), br.data.begin() + static_cast<std::ptrdiff_t>(br.bytePos),
               br.data.begin() + static_cast<std::ptrdiff_t>(br.bytePos + len));
    br.bytePos += len;
    return true;
}

// Decodes one Huffman-compressed block (fixed or dynamic tables) into `out`,
// resolving LZ77 length/distance back-references against the output produced so
// far. Returns on the end-of-block symbol (256).
bool inflateHuffman(BitReader& br, Bytes& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        const int sym = lit.decode(br);
        if (br.error || sym < 0) {
            return false;
        }
        if (sym == 256) {
            return true;
        }
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
            continue;
        }
        const std::size_t lidx = static_cast<std::size_t>(sym - 257);
        if (lidx >= kLengthBase.size()) {
            return false;
        }
        const unsigned length = static_cast<unsigned>(kLengthBase[lidx]) +
                                br.getBits(static_cast<unsigned>(kLengthExtra[lidx]));
        const int dsym = dist.decode(br);
        if (br.error || dsym < 0 || dsym >= static_cast<int>(kDistBase.size())) {
            return false;
        }
        const std::size_t didx = static_cast<std::size_t>(dsym);
        const std::size_t distance = static_cast<std::size_t>(kDistBase[didx]) +
                                     br.getBits(static_cast<unsigned>(kDistExtra[didx]));
        if (distance == 0 || distance > out.size()) {
            return false;
        }
        const std::size_t from = out.size() - distance;
        for (unsigned i = 0; i < length; ++i) {
            out.push_back(out[from + i]);
        }
        if (br.error) {
            return false;
        }
    }
}

// Reads a dynamic-block (BTYPE=10) header: the code-length code lengths, then
// the run-length-encoded literal/length + distance code lengths, and builds the
// two Huffman tables from them.
bool buildDynamicTables(BitReader& br, Huffman& lit, Huffman& dist) {
    const unsigned hlit = br.getBits(5) + 257;
    const unsigned hdist = br.getBits(5) + 1;
    const unsigned hclen = br.getBits(4) + 4;
    if (br.error || hlit > 286 || hdist > 30) {
        return false;
    }
    static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::vector<int> clLengths(19, 0);
    for (unsigned i = 0; i < hclen; ++i) {
        clLengths[static_cast<std::size_t>(order[i])] = static_cast<int>(br.getBits(3));
    }
    if (br.error) {
        return false;
    }
    Huffman clHuff;
    if (!clHuff.build(clLengths)) {
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(hlit) + static_cast<std::size_t>(hdist);
    std::vector<int> lengths;
    lengths.reserve(total);
    while (lengths.size() < total) {
        const int sym = clHuff.decode(br);
        if (br.error || sym < 0) {
            return false;
        }
        if (sym < 16) {
            lengths.push_back(sym);
        } else if (sym == 16) {
            if (lengths.empty()) {
                return false;
            }
            const unsigned rep = br.getBits(2) + 3;
            const int prev = lengths.back();
            for (unsigned i = 0; i < rep; ++i) {
                lengths.push_back(prev);
            }
        } else if (sym == 17) {
            const unsigned rep = br.getBits(3) + 3;
            lengths.insert(lengths.end(), rep, 0);
        } else {  // sym == 18
            const unsigned rep = br.getBits(7) + 11;
            lengths.insert(lengths.end(), rep, 0);
        }
    }
    if (br.error || lengths.size() != total) {  // a repeat must not overrun the table
        return false;
    }
    std::vector<int> litLengths(lengths.begin(),
                                lengths.begin() + static_cast<std::ptrdiff_t>(hlit));
    std::vector<int> distLengths(lengths.begin() + static_cast<std::ptrdiff_t>(hlit),
                                 lengths.end());
    return lit.build(litLengths) && dist.build(distLengths);
}

// Validates the zlib wrapper (CMF/FLG, no preset dictionary) and inflates every
// DEFLATE block (stored / fixed / dynamic Huffman), then checks the trailing
// Adler-32 over the decompressed data.
std::optional<Bytes> zlibInflate(const Bytes& z) {
    if (z.size() < 6) {
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
    const Huffman fixedLit = fixedLiteralTable();
    const Huffman fixedDist = fixedDistanceTable();
    BitReader br(z, 2);
    Bytes out;
    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = br.getBit() != 0u;
        const unsigned btype = br.getBits(2);
        if (br.error) {
            return std::nullopt;
        }
        if (btype == 0u) {
            if (!inflateStored(br, out)) {
                return std::nullopt;
            }
        } else if (btype == 1u) {
            if (!inflateHuffman(br, out, fixedLit, fixedDist)) {
                return std::nullopt;
            }
        } else if (btype == 2u) {
            Huffman lit;
            Huffman dist;
            if (!buildDynamicTables(br, lit, dist) || !inflateHuffman(br, out, lit, dist)) {
                return std::nullopt;
            }
        } else {  // btype == 3 is reserved
            return std::nullopt;
        }
    }
    br.alignToByte();
    if (br.bytePos + 4 > z.size()) {
        return std::nullopt;
    }
    if (readU32BE(z, br.bytePos) != detail::adler32(out.data(), out.size())) {
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// PNG chunk parsing
// ---------------------------------------------------------------------------

// Parsed IHDR plus ancillary data (palette / transparency) needed to decode.
struct Header {
    int width = 0;
    int height = 0;
    std::uint8_t colourType = 0;
    int channels = 0;  // samples per pixel in the raw scanline data
};

// Validates a 13-byte IHDR payload. Accepts 8-bit colour types 0 (grayscale),
// 2 (RGB), 3 (palette), 6 (RGBA), non-interlaced only. 16-bit depth and
// interlaced images are rejected (nullopt).
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
    int channels = 0;
    switch (colourType) {
        case 0:
            channels = 1;
            break;  // grayscale
        case 2:
            channels = 3;
            break;  // RGB
        case 3:
            channels = 1;
            break;  // palette index
        case 6:
            channels = 4;
            break;  // RGBA
        default:
            return std::nullopt;
    }
    Header hdr;
    hdr.width = static_cast<int>(w);
    hdr.height = static_cast<int>(h);
    hdr.colourType = colourType;
    hdr.channels = channels;
    return hdr;
}

// Walks the chunk stream, validating each chunk's CRC-32 over type+data. Fills
// the header and collects IDAT, PLTE and tRNS payloads. Returns false on any
// truncation, CRC mismatch, or malformed IHDR.
bool collectChunks(const Bytes& b, Header& header, Bytes& idat, Bytes& plte, Bytes& trns) {
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
        const auto begin = b.begin() + static_cast<std::ptrdiff_t>(dataStart);
        const auto end = b.begin() + static_cast<std::ptrdiff_t>(dataStart + len);
        if (std::equal(type, type + 4, "IHDR")) {
            const auto hdr = parseIhdr(Bytes(begin, end));
            if (!hdr) {
                return false;
            }
            header = *hdr;
            sawIhdr = true;
        } else if (std::equal(type, type + 4, "PLTE")) {
            plte.assign(begin, end);
        } else if (std::equal(type, type + 4, "tRNS")) {
            trns.assign(begin, end);
        } else if (std::equal(type, type + 4, "IDAT")) {
            idat.insert(idat.end(), begin, end);
        } else if (std::equal(type, type + 4, "IEND")) {
            return sawIhdr;
        }
        pos = dataStart + len + 4;
    }
    return false;  // ran out of bytes before IEND
}

// ---------------------------------------------------------------------------
// Scanline filter reconstruction (RFC 2083 / PNG spec 6.x)
// ---------------------------------------------------------------------------

int paethPredictor(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return (pb <= pc) ? b : c;
}

// Reverses the per-scanline filters (0 None, 1 Sub, 2 Up, 3 Average, 4 Paeth),
// stripping the leading filter byte of each row. `bpp` is bytes per pixel.
std::optional<Bytes> reconstruct(const Bytes& raw, std::size_t rowBytes, std::size_t bpp,
                                 std::size_t h) {
    if (raw.size() != (rowBytes + 1) * h) {
        return std::nullopt;
    }
    Bytes out(rowBytes * h);
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t filter = raw[y * (rowBytes + 1)];
        if (filter > 4) {
            return std::nullopt;
        }
        const std::size_t srcRow = y * (rowBytes + 1) + 1;
        const std::size_t curRow = y * rowBytes;
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const int left = (x >= bpp) ? out[curRow + x - bpp] : 0;
            const int up = (y > 0) ? out[curRow - rowBytes + x] : 0;
            const int upLeft = (y > 0 && x >= bpp) ? out[curRow - rowBytes + x - bpp] : 0;
            int value = raw[srcRow + x];
            switch (filter) {
                case 1:
                    value += left;
                    break;
                case 2:
                    value += up;
                    break;
                case 3:
                    value += (left + up) / 2;
                    break;
                case 4:
                    value += paethPredictor(left, up, upLeft);
                    break;
                default:  // 0 None
                    break;
            }
            out[curRow + x] = static_cast<std::uint8_t>(value & 0xff);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sample expansion to normalised float RGB/RGBA
// ---------------------------------------------------------------------------

constexpr float kInv255 = 1.0f / 255.0f;

std::optional<std::vector<float>> expandGrayscale(const Bytes& rows, std::size_t count,
                                                  int& outChannels) {
    outChannels = 3;
    std::vector<float> pixels(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        const float v = static_cast<float>(rows[i]) * kInv255;
        pixels[i * 3 + 0] = v;
        pixels[i * 3 + 1] = v;
        pixels[i * 3 + 2] = v;
    }
    return pixels;
}

std::optional<std::vector<float>> expandDirect(const Bytes& rows, int channels, int& outChannels) {
    outChannels = channels;
    std::vector<float> pixels(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        pixels[i] = static_cast<float>(rows[i]) * kInv255;
    }
    return pixels;
}

std::optional<std::vector<float>> expandPalette(const Bytes& rows, std::size_t count,
                                                const Bytes& plte, const Bytes& trns,
                                                int& outChannels) {
    if (plte.empty() || plte.size() % 3 != 0) {
        return std::nullopt;
    }
    const std::size_t entries = plte.size() / 3;
    const bool hasAlpha = !trns.empty();
    outChannels = hasAlpha ? 4 : 3;
    const std::size_t oc = static_cast<std::size_t>(outChannels);
    std::vector<float> pixels(count * oc);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t idx = rows[i];
        if (idx >= entries) {
            return std::nullopt;
        }
        pixels[i * oc + 0] = static_cast<float>(plte[idx * 3 + 0]) * kInv255;
        pixels[i * oc + 1] = static_cast<float>(plte[idx * 3 + 1]) * kInv255;
        pixels[i * oc + 2] = static_cast<float>(plte[idx * 3 + 2]) * kInv255;
        if (hasAlpha) {
            const float a = (idx < trns.size()) ? static_cast<float>(trns[idx]) * kInv255 : 1.0f;
            pixels[i * oc + 3] = a;
        }
    }
    return pixels;
}

// Reconstructs scanlines then expands them to normalised float RGB/RGBA
// according to the colour type. `outChannels` receives 3 or 4.
std::optional<std::vector<float>> decodePixels(const Bytes& raw, const Header& hdr,
                                               const Bytes& plte, const Bytes& trns,
                                               int& outChannels) {
    const std::size_t w = static_cast<std::size_t>(hdr.width);
    const std::size_t h = static_cast<std::size_t>(hdr.height);
    const std::size_t bpp = static_cast<std::size_t>(hdr.channels);
    const std::size_t rowBytes = w * bpp;
    const auto rows = reconstruct(raw, rowBytes, bpp, h);
    if (!rows) {
        return std::nullopt;
    }
    const std::size_t count = w * h;
    switch (hdr.colourType) {
        case 0:
            return expandGrayscale(*rows, count, outChannels);
        case 2:
            return expandDirect(*rows, 3, outChannels);
        case 6:
            return expandDirect(*rows, 4, outChannels);
        case 3:
            return expandPalette(*rows, count, plte, trns, outChannels);
        default:
            return std::nullopt;
    }
}

}  // namespace

std::optional<LoadedImage> loadPng(const std::string& path) {
    const Bytes bytes = readFileBytes(path);
    if (!hasPngSignature(bytes)) {
        return std::nullopt;
    }
    Header header;
    Bytes idat;
    Bytes plte;
    Bytes trns;
    if (!collectChunks(bytes, header, idat, plte, trns)) {
        return std::nullopt;
    }
    const auto raw = zlibInflate(idat);
    if (!raw) {
        return std::nullopt;
    }
    int outChannels = 0;
    auto pixels = decodePixels(*raw, header, plte, trns, outChannels);
    if (!pixels) {
        return std::nullopt;
    }
    LoadedImage image;
    image.width = header.width;
    image.height = header.height;
    image.channels = outChannels;
    image.pixels = std::move(*pixels);
    return image;
}

}  // namespace cyber::imageio
