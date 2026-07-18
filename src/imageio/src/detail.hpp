#pragma once

// Internal helpers shared by the imageio encoders: checksums (CRC-32, Adler-32),
// little/big-endian byte appenders, and a raw file writer. Header-only and kept
// out of the public include tree on purpose.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace cyber::imageio::detail {

using Bytes = std::vector<std::uint8_t>;

// --- CRC-32 (IEEE 802.3, reflected 0xEDB88320) used by PNG chunks and ZIP -----

inline const std::array<std::uint32_t, 256>& crcTable() {
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
    return table;
}

// Running CRC update; pass 0xffffffff to start, xor the result with 0xffffffff.
inline std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t len) {
    const auto& table = crcTable();
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint32_t idx = (crc ^ data[i]) & 0xffu;
        crc = table[static_cast<std::size_t>(idx)] ^ (crc >> 8);
    }
    return crc;
}

// Complete CRC-32 over a buffer (initial and final xor with 0xffffffff).
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    return crc32Update(0xffffffffu, data, len) ^ 0xffffffffu;
}
inline std::uint32_t crc32(const Bytes& data) { return crc32(data.data(), data.size()); }

// --- Adler-32 used by the zlib stream inside PNG IDAT -------------------------

inline std::uint32_t adler32(const std::uint8_t* data, std::size_t len) {
    constexpr std::uint32_t kMod = 65521u;
    std::uint32_t a = 1u;
    std::uint32_t b = 0u;
    for (std::size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

// --- Byte appenders -----------------------------------------------------------

inline void appendU16LE(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
}

inline void appendU32LE(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

inline void appendU32BE(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
}

inline void appendU64LE(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
}

inline void appendI32LE(Bytes& out, std::int32_t v) {
    appendU32LE(out, static_cast<std::uint32_t>(v));
}

inline void appendFloatLE(Bytes& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    appendU32LE(out, bits);
}

inline void appendBytes(Bytes& out, const Bytes& src) {
    out.insert(out.end(), src.begin(), src.end());
}

// Appends a NUL-terminated string (OpenEXR attribute name/type convention).
inline void appendCString(Bytes& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

// --- Raw file writer ----------------------------------------------------------

inline bool writeFile(const std::string& path, const Bytes& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(file);
}

}  // namespace cyber::imageio::detail
