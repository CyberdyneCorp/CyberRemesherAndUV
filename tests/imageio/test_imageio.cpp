#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cyber/imageio/exr.hpp"
#include "cyber/imageio/png.hpp"
#include "cyber/imageio/zip.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

Bytes readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::uint32_t readU32BE(const Bytes& b, std::size_t o) {
    return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16) |
           (static_cast<std::uint32_t>(b[o + 2]) << 8) | static_cast<std::uint32_t>(b[o + 3]);
}

std::uint32_t readU32LE(const Bytes& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

std::uint16_t readU16LE(const Bytes& b, std::size_t o) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(b[o]) |
                                      (static_cast<std::uint32_t>(b[o + 1]) << 8));
}

// Independent reference CRC-32 to cross-check the encoder's output.
std::uint32_t refCrc32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) != 0u ? (0xedb88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xffffffffu;
}

}  // namespace

TEST_CASE("PNG has valid signature, IHDR and per-chunk CRCs") {
    const std::string path = tempPath("cyber_imageio_test.png");
    const std::array<std::uint8_t, 2 * 2 * 4> px = {
        255, 0,   0,   255,  // red
        0,   255, 0,   255,  // green
        0,   0,   255, 255,  // blue
        255, 255, 255, 128,  // white, half alpha
    };
    REQUIRE(cyber::imageio::writePng(path, 2, 2, 4, px.data()));

    const Bytes bytes = readFile(path);
    REQUIRE(bytes.size() > 8);

    const std::array<std::uint8_t, 8> sig = {137, 80, 78, 71, 13, 10, 26, 10};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(bytes[i] == sig[i]);
    }

    // First chunk must be IHDR with the correct dimensions and color type.
    CHECK(readU32BE(bytes, 8) == 13u);  // IHDR length
    CHECK(bytes[12] == 'I');
    CHECK(bytes[13] == 'H');
    CHECK(bytes[14] == 'D');
    CHECK(bytes[15] == 'R');
    CHECK(readU32BE(bytes, 16) == 2u);  // width
    CHECK(readU32BE(bytes, 20) == 2u);  // height
    CHECK(bytes[24] == 8);              // bit depth
    CHECK(bytes[25] == 6);              // color type RGBA

    // Walk every chunk and validate its CRC over type+data.
    std::size_t pos = 8;
    bool sawIend = false;
    while (pos + 12 <= bytes.size()) {
        const std::uint32_t len = readU32BE(bytes, pos);
        const std::size_t crcPos = pos + 8 + len;
        REQUIRE(crcPos + 4 <= bytes.size());
        const std::uint32_t stored = readU32BE(bytes, crcPos);
        const std::uint32_t computed = refCrc32(bytes.data() + pos + 4, 4 + len);
        CHECK(stored == computed);
        if (std::memcmp(bytes.data() + pos + 4, "IEND", 4) == 0) {
            sawIend = true;
            break;
        }
        pos = crcPos + 4;
    }
    CHECK(sawIend);
}

TEST_CASE("PNG rejects invalid arguments") {
    const std::string path = tempPath("cyber_imageio_bad.png");
    const std::array<std::uint8_t, 3> px = {1, 2, 3};
    CHECK_FALSE(cyber::imageio::writePng(path, 0, 1, 3, px.data()));
    CHECK_FALSE(cyber::imageio::writePng(path, 1, 1, 2, px.data()));
    CHECK_FALSE(cyber::imageio::writePng(path, 1, 1, 3, nullptr));
}

TEST_CASE("EXR has correct magic and version") {
    const std::string path = tempPath("cyber_imageio_test.exr");
    const std::array<float, 2 * 2 * 3> px = {
        1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.5f,
    };
    REQUIRE(cyber::imageio::writeExr(path, 2, 2, 3, px.data()));

    const Bytes bytes = readFile(path);
    REQUIRE(bytes.size() > 8);
    CHECK(readU32LE(bytes, 0) == 0x01312f76u);  // OpenEXR magic
    CHECK((readU32LE(bytes, 4) & 0xffu) == 2u);  // version number
    // Header must contain the required channels attribute.
    bool sawChannels = false;
    for (std::size_t i = 0; i + 8 < bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, "channels", 8) == 0) {
            sawChannels = true;
            break;
        }
    }
    CHECK(sawChannels);
}

TEST_CASE("ZIP round-trips entries with valid EOCD and CRCs") {
    const std::string path = tempPath("cyber_imageio_test.zip");
    const Bytes a = {'h', 'e', 'l', 'l', 'o'};
    const Bytes b = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    cyber::imageio::ZipWriter zip;
    zip.add("a.txt", a);
    zip.add("b.bin", b);
    REQUIRE(zip.entryCount() == 2);
    REQUIRE(zip.finish(path));

    const Bytes bytes = readFile(path);
    REQUIRE(bytes.size() > 22);

    // Locate the end-of-central-directory record (fixed 22-byte tail, no comment).
    const std::size_t eocd = bytes.size() - 22;
    CHECK(readU32LE(bytes, eocd) == 0x06054b50u);
    CHECK(readU16LE(bytes, eocd + 8) == 2);   // entries on this disk
    CHECK(readU16LE(bytes, eocd + 10) == 2);  // total entries

    const std::uint32_t cdSize = readU32LE(bytes, eocd + 12);
    const std::uint32_t cdOffset = readU32LE(bytes, eocd + 16);
    CHECK(cdOffset + cdSize == eocd);

    // Verify each local file header: signature, stored method, and that the
    // stored CRC matches an independent CRC of the entry payload.
    const std::array<Bytes, 2> payloads = {a, b};
    std::size_t pos = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        REQUIRE(readU32LE(bytes, pos) == 0x04034b50u);  // local file header signature
        CHECK(readU16LE(bytes, pos + 8) == 0);          // stored, no compression
        const std::uint32_t crc = readU32LE(bytes, pos + 14);
        const std::uint32_t size = readU32LE(bytes, pos + 22);
        const std::uint16_t nameLen = readU16LE(bytes, pos + 26);
        const std::uint16_t extraLen = readU16LE(bytes, pos + 28);
        const std::size_t dataStart = pos + 30 + nameLen + extraLen;
        CHECK(size == payloads[i].size());
        CHECK(crc == refCrc32(payloads[i].data(), payloads[i].size()));
        CHECK(std::memcmp(bytes.data() + dataStart, payloads[i].data(), size) == 0);
        pos = dataStart + size;
    }
    // After both local entries we should be at the start of the central directory.
    CHECK(pos == cdOffset);
    CHECK(readU32LE(bytes, pos) == 0x02014b50u);  // central directory signature
}
