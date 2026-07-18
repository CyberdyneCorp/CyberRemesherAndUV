#pragma once

#include <cstddef>
#include <string>

#include "cyber/net/protocol.hpp"

// Thin POSIX-socket helpers shared by the server and client. Isolated here so
// the platform code lives in one place (a Windows Winsock shim would slot in
// behind the same three functions).
namespace cyber::net::detail {

// Writes all `n` bytes, looping over partial sends; false on error/disconnect.
bool writeAll(int fd, const char* data, std::size_t n);

// Length-prefixes and sends one payload.
bool sendFrame(int fd, const std::string& payload);

// Reads until `decoder` yields one full frame; false on disconnect or when the
// decoder latches an oversize/malformed failure.
bool recvFrame(int fd, FrameDecoder& decoder, std::string& out);

}  // namespace cyber::net::detail
