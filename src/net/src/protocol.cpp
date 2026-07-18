#include "cyber/net/protocol.hpp"

#include <cstring>

namespace cyber::net {

std::string encodeFrame(const std::string& payload) {
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.resize(payload.size() + 4);
    frame[0] = static_cast<char>((n >> 24) & 0xFFu);
    frame[1] = static_cast<char>((n >> 16) & 0xFFu);
    frame[2] = static_cast<char>((n >> 8) & 0xFFu);
    frame[3] = static_cast<char>(n & 0xFFu);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    return frame;
}

void FrameDecoder::feed(const char* data, std::size_t n) {
    if (!m_failed) {
        m_buffer.append(data, n);
    }
}

bool FrameDecoder::next(std::string& out) {
    if (m_failed || m_buffer.size() < 4) {
        return false;
    }
    const auto b0 = static_cast<unsigned char>(m_buffer[0]);
    const auto b1 = static_cast<unsigned char>(m_buffer[1]);
    const auto b2 = static_cast<unsigned char>(m_buffer[2]);
    const auto b3 = static_cast<unsigned char>(m_buffer[3]);
    const std::size_t length = (static_cast<std::size_t>(b0) << 24) |
                               (static_cast<std::size_t>(b1) << 16) |
                               (static_cast<std::size_t>(b2) << 8) | static_cast<std::size_t>(b3);
    if (length > kMaxMessageBytes) {
        m_failed = true;  // refuse the connection rather than allocate it
        return false;
    }
    if (m_buffer.size() < length + 4) {
        return false;  // wait for more bytes
    }
    out.assign(m_buffer, 4, length);
    m_buffer.erase(0, length + 4);
    return true;
}

}  // namespace cyber::net
