#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Wire protocol for the local DCC bridge (network-bridge spec). Framing is a
// 4-byte big-endian payload length followed by a UTF-8 JSON payload; every
// connection opens with a version handshake. The codec here is transport-
// agnostic and pure, so it is unit-tested without sockets.
namespace cyber::net {

inline constexpr std::uint32_t kProtocolVersion = 1;

// Hard ceiling on a single message. Anything larger is rejected without
// allocating it (oversized-message hardening — spec "Protocol versioning and
// safety").
inline constexpr std::size_t kMaxMessageBytes = 256u * 1024u * 1024u;

// Mesh payload as it travels on the wire: flat coordinate arrays plus a
// variable-arity face list, with optional per-vertex UVs and colors. The
// bridge transports geometry; mapping it onto the document's attribute system
// is the document layer's job (group 8), so this stays deliberately decoupled
// from the core Mesh attribute schema.
struct WireMesh {
    std::vector<float> positions;              // xyz per vertex, size = 3 * V
    std::vector<std::vector<std::uint32_t>> faces;  // indices per face (n-gon)
    std::vector<float> uvs;                    // uv per vertex, size = 2 * V or empty
    std::vector<float> colors;                 // rgb per vertex, size = 3 * V or empty

    [[nodiscard]] std::size_t vertexCount() const { return positions.size() / 3; }
    friend bool operator==(const WireMesh& a, const WireMesh& b) {
        return a.positions == b.positions && a.faces == b.faces && a.uvs == b.uvs &&
               a.colors == b.colors;
    }
};

// Prefix `payload` with its big-endian length. Caller guarantees the payload
// is within kMaxMessageBytes.
[[nodiscard]] std::string encodeFrame(const std::string& payload);

// Incremental frame reader: feed received bytes, pop complete frames. On an
// oversized length it latches failure and refuses further frames, so the
// server drops the connection instead of allocating attacker-chosen memory.
class FrameDecoder {
public:
    void feed(const char* data, std::size_t n);
    // Pops one complete frame into `out`; returns false when none is ready.
    [[nodiscard]] bool next(std::string& out);
    [[nodiscard]] bool failed() const { return m_failed; }

private:
    std::string m_buffer;
    bool m_failed = false;
};

}  // namespace cyber::net
