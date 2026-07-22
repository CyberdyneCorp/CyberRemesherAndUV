#include <doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cyber/imageio/load.hpp"

// Exercises the GENERAL 8-bit PNG decoder in src/load.cpp against streams the
// project's own writer never emits: fixed-Huffman DEFLATE with an LZ77 back
// reference, every scanline filter (Sub/Up/Average/Paeth), palette+tRNS, and
// rejection of 16-bit / interlaced headers. Each PNG is hand-encoded in the
// test so no external tooling is required. (The orchestrator additionally
// verifies against a real PIL-written dynamic-Huffman PNG.)

namespace {

using Bytes = std::vector<std::uint8_t>;

// --- checksums -------------------------------------------------------------

std::uint32_t crc32Bytes(const Bytes& data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (const std::uint8_t byte : data) {
        crc = table[static_cast<std::size_t>((crc ^ byte) & 0xffu)] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

std::uint32_t adler32Bytes(const Bytes& data) {
    constexpr std::uint32_t kMod = 65521u;
    std::uint32_t a = 1u;
    std::uint32_t b = 0u;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

void appendU32BE(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
}

// --- PNG assembly ----------------------------------------------------------

void appendChunk(Bytes& out, const char type[5], const Bytes& payload) {
    appendU32BE(out, static_cast<std::uint32_t>(payload.size()));
    Bytes typed;
    for (int i = 0; i < 4; ++i) {
        typed.push_back(static_cast<std::uint8_t>(type[i]));
    }
    typed.insert(typed.end(), payload.begin(), payload.end());
    out.insert(out.end(), typed.begin(), typed.end());
    appendU32BE(out, crc32Bytes(typed));
}

Bytes ihdr(std::uint32_t w, std::uint32_t h, std::uint8_t bitDepth, std::uint8_t colourType,
           std::uint8_t interlace) {
    Bytes p;
    appendU32BE(p, w);
    appendU32BE(p, h);
    p.push_back(bitDepth);
    p.push_back(colourType);
    p.push_back(0);  // compression
    p.push_back(0);  // filter method
    p.push_back(interlace);
    return p;
}

Bytes buildPng(std::uint32_t w, std::uint32_t h, std::uint8_t bitDepth, std::uint8_t colourType,
               std::uint8_t interlace, const Bytes& idat, const Bytes& plte, const Bytes& trns) {
    static const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    Bytes out(sig, sig + 8);
    appendChunk(out, "IHDR", ihdr(w, h, bitDepth, colourType, interlace));
    if (!plte.empty()) {
        appendChunk(out, "PLTE", plte);
    }
    if (!trns.empty()) {
        appendChunk(out, "tRNS", trns);
    }
    appendChunk(out, "IDAT", idat);
    appendChunk(out, "IEND", Bytes{});
    return out;
}

// --- zlib wrappers ---------------------------------------------------------

// Wraps raw deflate bytes with a zlib header (0x78 0x01) and Adler-32 of the
// decompressed payload.
Bytes zlibWrap(const Bytes& deflate, const Bytes& decompressed) {
    Bytes z{0x78, 0x01};
    z.insert(z.end(), deflate.begin(), deflate.end());
    appendU32BE(z, adler32Bytes(decompressed));
    return z;
}

// A single STORED (uncompressed) final DEFLATE block.
Bytes deflateStored(const Bytes& data) {
    Bytes d;
    d.push_back(0x01);  // BFINAL=1, BTYPE=00
    const std::uint16_t len = static_cast<std::uint16_t>(data.size());
    const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
    d.push_back(static_cast<std::uint8_t>(len & 0xffu));
    d.push_back(static_cast<std::uint8_t>((len >> 8) & 0xffu));
    d.push_back(static_cast<std::uint8_t>(nlen & 0xffu));
    d.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xffu));
    d.insert(d.end(), data.begin(), data.end());
    return d;
}

Bytes zlibStored(const Bytes& data) { return zlibWrap(deflateStored(data), data); }

// --- fixed-Huffman DEFLATE bit encoder ------------------------------------

// LSB-first bit sink: integer fields go LSB-first, Huffman codes MSB-first
// (RFC 1951 §3.1.1).
struct BitWriter {
    Bytes bytes;
    unsigned cur = 0;
    unsigned nbits = 0;

    void bit(unsigned b) {
        cur |= (b & 1u) << nbits;
        ++nbits;
        if (nbits == 8) {
            bytes.push_back(static_cast<std::uint8_t>(cur));
            cur = 0;
            nbits = 0;
        }
    }
    void bitsLSB(unsigned v, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            bit((v >> i) & 1u);
        }
    }
    void huff(unsigned code, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            bit((code >> (n - 1 - i)) & 1u);
        }
    }
    void flush() {
        if (nbits > 0) {
            bytes.push_back(static_cast<std::uint8_t>(cur));
            cur = 0;
            nbits = 0;
        }
    }
};

constexpr std::array<int, 29> kLengthBase = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                             15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistBase = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Fixed-Huffman literal (0..143 → 8-bit code 0x30+lit).
void emitLiteral(BitWriter& bw, std::uint8_t lit) {
    REQUIRE(lit <= 143);  // keeps the encoder to the 8-bit code range
    bw.huff(0x30u + lit, 8);
}

void emitEndOfBlock(BitWriter& bw) { bw.huff(0x00u, 7); }  // symbol 256

void emitMatch(BitWriter& bw, int length, int distance) {
    int li = 0;
    while (li < 28 && length >= kLengthBase[static_cast<std::size_t>(li) + 1]) {
        ++li;
    }
    const int lsym = 257 + li;  // fixed table: 257..279 → 7-bit code (sym-256)
    bw.huff(static_cast<unsigned>(lsym - 256), 7);
    bw.bitsLSB(static_cast<unsigned>(length - kLengthBase[static_cast<std::size_t>(li)]),
               static_cast<unsigned>(kLengthExtra[static_cast<std::size_t>(li)]));
    int di = 0;
    while (di < 29 && distance >= kDistBase[static_cast<std::size_t>(di) + 1]) {
        ++di;
    }
    bw.huff(static_cast<unsigned>(di), 5);  // 5-bit fixed distance code
    bw.bitsLSB(static_cast<unsigned>(distance - kDistBase[static_cast<std::size_t>(di)]),
               static_cast<unsigned>(kDistExtra[static_cast<std::size_t>(di)]));
}

// --- forward scanline filter (encode side) --------------------------------

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return (pb <= pc) ? b : c;
}

// Applies one filter type uniformly to an image, producing filter-prefixed
// scanlines (the inverse of the decoder's reconstruct()).
Bytes applyFilter(const Bytes& img, std::size_t w, std::size_t h, std::size_t bpp,
                  std::uint8_t ft) {
    const std::size_t rowBytes = w * bpp;
    Bytes out;
    for (std::size_t y = 0; y < h; ++y) {
        out.push_back(ft);
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const int raw = img[y * rowBytes + x];
            const int a = (x >= bpp) ? img[y * rowBytes + x - bpp] : 0;
            const int b = (y > 0) ? img[(y - 1) * rowBytes + x] : 0;
            const int c = (y > 0 && x >= bpp) ? img[(y - 1) * rowBytes + x - bpp] : 0;
            int val = raw;
            switch (ft) {
                case 1:
                    val = raw - a;
                    break;
                case 2:
                    val = raw - b;
                    break;
                case 3:
                    val = raw - (a + b) / 2;
                    break;
                case 4:
                    val = raw - paeth(a, b, c);
                    break;
                default:
                    break;
            }
            out.push_back(static_cast<std::uint8_t>(val & 0xff));
        }
    }
    return out;
}

// --- file I/O --------------------------------------------------------------

std::string writeTemp(const char* name, const Bytes& data) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

}  // namespace

TEST_CASE("loadPng decodes a fixed-Huffman stream with an LZ77 back-reference") {
    // 8x1 grayscale, filter None. Raw samples repeat with period 4 so the second
    // half is a length-4 / distance-4 match rather than literals.
    const Bytes samples = {1, 2, 3, 4, 1, 2, 3, 4};
    Bytes decompressed = {0};  // filter byte
    decompressed.insert(decompressed.end(), samples.begin(), samples.end());

    BitWriter bw;
    bw.bitsLSB(1, 1);  // BFINAL = 1
    bw.bitsLSB(1, 2);  // BTYPE = 01 (fixed Huffman)
    emitLiteral(bw, 0);
    emitLiteral(bw, 1);
    emitLiteral(bw, 2);
    emitLiteral(bw, 3);
    emitLiteral(bw, 4);
    emitMatch(bw, 4, 4);  // copies samples[0..3] → samples[4..7]
    emitEndOfBlock(bw);
    bw.flush();

    const Bytes png = buildPng(8, 1, 8, 0, 0, zlibWrap(bw.bytes, decompressed), {}, {});
    const std::string path = writeTemp("cyber_general_fixed_lz77.png", png);

    const auto loaded = cyber::imageio::loadPng(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->width == 8);
    CHECK(loaded->height == 1);
    CHECK(loaded->channels == 3);  // grayscale expands to RGB
    REQUIRE(loaded->pixels.size() == static_cast<std::size_t>(8 * 3));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float expected = static_cast<float>(samples[i]) / 255.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            CHECK(loaded->pixels[i * 3 + c] == doctest::Approx(expected));
        }
    }
}

TEST_CASE("loadPng reconstructs every scanline filter type") {
    constexpr std::size_t w = 3;
    constexpr std::size_t h = 3;
    constexpr std::size_t bpp = 3;  // RGB
    Bytes img(w * h * bpp);
    for (std::size_t i = 0; i < img.size(); ++i) {
        img[i] = static_cast<std::uint8_t>((i * 23 + 5) & 0xff);
    }

    for (int fti = 0; fti <= 4; ++fti) {
        CAPTURE(fti);
        const std::uint8_t ft = static_cast<std::uint8_t>(fti);
        const Bytes filtered = applyFilter(img, w, h, bpp, ft);
        const Bytes png = buildPng(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 8,
                                   2, 0, zlibStored(filtered), {}, {});
        const std::string path = writeTemp(
            (std::string("cyber_general_filter_") + std::to_string(fti) + ".png").c_str(), png);
        const auto loaded = cyber::imageio::loadPng(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->channels == 3);
        REQUIRE(loaded->pixels.size() == img.size());
        for (std::size_t i = 0; i < img.size(); ++i) {
            const float expected = static_cast<float>(img[i]) / 255.0f;
            CHECK(loaded->pixels[i] == doctest::Approx(expected));
        }
    }
}

TEST_CASE("loadPng expands a paletted image and honours tRNS") {
    // 2x2 palette image, indices 0..3, filter None.
    const Bytes plte = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    const Bytes trns = {0, 128, 255};  // idx3 defaults to opaque
    Bytes decompressed;
    // Two rows of two indices each, filter byte 0 per row.
    decompressed.insert(decompressed.end(), {0, 0, 1});
    decompressed.insert(decompressed.end(), {0, 2, 3});

    const Bytes png = buildPng(2, 2, 8, 3, 0, zlibStored(decompressed), plte, trns);
    const std::string path = writeTemp("cyber_general_palette.png", png);
    const auto loaded = cyber::imageio::loadPng(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->channels == 4);  // tRNS present → RGBA
    REQUIRE(loaded->pixels.size() == static_cast<std::size_t>(2 * 2 * 4));

    const std::array<std::array<float, 4>, 4> expected = {{
        {255.0f / 255, 0, 0, 0.0f / 255},                  // idx0
        {0, 255.0f / 255, 0, 128.0f / 255},                // idx1
        {0, 0, 255.0f / 255, 255.0f / 255},                // idx2
        {255.0f / 255, 255.0f / 255, 255.0f / 255, 1.0f},  // idx3, no tRNS entry
    }};
    for (std::size_t px = 0; px < 4; ++px) {
        for (std::size_t c = 0; c < 4; ++c) {
            CHECK(loaded->pixels[px * 4 + c] == doctest::Approx(expected[px][c]));
        }
    }
}

TEST_CASE("loadPng rejects 16-bit and interlaced PNGs cleanly") {
    const Bytes idat = zlibStored(Bytes(64, 0));
    const Bytes deep = buildPng(2, 2, 16, 2, 0, idat, {}, {});
    const Bytes interlaced = buildPng(2, 2, 8, 2, 1, idat, {}, {});
    CHECK_FALSE(cyber::imageio::loadPng(writeTemp("cyber_general_16bit.png", deep)).has_value());
    CHECK_FALSE(
        cyber::imageio::loadPng(writeTemp("cyber_general_interlaced.png", interlaced)).has_value());
}

TEST_CASE("loadPng rejects a corrupt Adler-32 trailer") {
    const Bytes filtered = {0, 10, 20, 30};  // 1x1 RGB row
    Bytes idat = zlibStored(filtered);
    idat.back() ^= 0xffu;  // corrupt the last Adler-32 byte
    const Bytes png = buildPng(1, 1, 8, 2, 0, idat, {}, {});
    CHECK_FALSE(cyber::imageio::loadPng(writeTemp("cyber_general_bad_adler.png", png)).has_value());
}
