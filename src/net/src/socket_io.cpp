#include "socket_io.hpp"

#include <sys/socket.h>
#include <unistd.h>

namespace cyber::net::detail {

namespace {
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // never raise SIGPIPE on a dead peer
#else
constexpr int kSendFlags = 0;
#endif
}  // namespace

bool writeAll(int fd, const char* data, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t k = ::send(fd, data + sent, n - sent, kSendFlags);
        if (k <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(k);
    }
    return true;
}

bool sendFrame(int fd, const std::string& payload) {
    const std::string frame = encodeFrame(payload);
    return writeAll(fd, frame.data(), frame.size());
}

bool recvFrame(int fd, FrameDecoder& decoder, std::string& out) {
    char buffer[8192];
    while (true) {
        if (decoder.next(out)) {
            return true;
        }
        if (decoder.failed()) {
            return false;
        }
        const ssize_t k = ::recv(fd, buffer, sizeof(buffer), 0);
        if (k <= 0) {
            return false;
        }
        decoder.feed(buffer, static_cast<std::size_t>(k));
    }
}

}  // namespace cyber::net::detail
