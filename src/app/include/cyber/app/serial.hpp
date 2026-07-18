#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Deterministic little-endian byte (de)serialization used by the application
// shell (application-shell spec, task 8): documents and the undo journal both
// persist through these primitives so the on-disk format is stable across
// platforms. Every read is bounds-checked; on underflow the reader latches an
// error flag and returns zero, so a truncated or malformed buffer never reads
// out of bounds.
namespace cyber::app {

class ByteWriter {
public:
    void u8(std::uint8_t v) { m_data.push_back(v); }

    void u32(std::uint32_t v) {
        m_data.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        m_data.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        m_data.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
        m_data.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    }

    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }

    void u64(std::uint64_t v) {
        for (int shift = 0; shift < 64; shift += 8) {
            m_data.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
        }
    }

    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }

    void bytes(std::span<const std::uint8_t> b) { m_data.insert(m_data.end(), b.begin(), b.end()); }

    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
        bytes({p, s.size()});
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const { return m_data; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(m_data); }

private:
    std::vector<std::uint8_t> m_data;
};

class ByteReader {
public:
    ByteReader() = default;
    explicit ByteReader(std::span<const std::uint8_t> data) : m_data(data) {}

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] std::size_t pos() const { return m_pos; }
    [[nodiscard]] std::size_t remaining() const { return m_data.size() - m_pos; }

    std::uint8_t u8() {
        if (!ensure(1)) {
            return 0;
        }
        return m_data[m_pos++];
    }

    std::uint32_t u32() {
        if (!ensure(4)) {
            return 0;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(m_data[m_pos]) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 8) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 16) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 3]) << 24);
        m_pos += 4;
        return v;
    }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    std::uint64_t u64() {
        if (!ensure(8)) {
            return 0;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(m_data[m_pos + i]) << (i * 8);
        }
        m_pos += 8;
        return v;
    }

    float f32() { return std::bit_cast<float>(u32()); }

    std::string str() {
        const std::uint32_t len = u32();
        if (!ensure(len)) {
            return {};
        }
        std::string s(reinterpret_cast<const char*>(&m_data[m_pos]), len);
        m_pos += len;
        return s;
    }

    // Returns a reader over the next `n` bytes and advances past them; on
    // underflow the returned reader is empty and this reader latches an error.
    [[nodiscard]] ByteReader sub(std::size_t n) {
        if (!ensure(n)) {
            return ByteReader{};
        }
        ByteReader r(m_data.subspan(m_pos, n));
        m_pos += n;
        return r;
    }

private:
    [[nodiscard]] bool ensure(std::size_t n) {
        if (!m_ok || m_pos + n > m_data.size()) {
            m_ok = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

}  // namespace cyber::app
