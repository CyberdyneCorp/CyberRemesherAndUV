#include "cyber/net/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>

#include "cyber/net/message.hpp"
#include "cyber/net/session.hpp"
#include "socket_io.hpp"

namespace cyber::net {

struct BridgeServer::Impl {
    explicit Impl(BridgeSession& s) : session(s) {}

    // One handler thread per connection, so concurrency is bounded: past this
    // the connection is closed immediately instead of spawning another stack.
    static constexpr std::size_t kMaxConnections = 64;

    BridgeSession& session;
    int listenFd = -1;
    // Self-pipe the accept thread also waits on, so stop() ends the wait at
    // once without closing the listening socket out from under that thread.
    int wakeReadFd = -1;
    int wakeWriteFd = -1;
    std::uint16_t boundPort = 0;
    std::atomic<bool> running{false};
    std::thread acceptThread;

    std::mutex connMutex;
    std::condition_variable allHandlersDone;
    std::size_t activeHandlers = 0;
    std::unordered_set<int> connFds;

    void serve(int fd) {
        FrameDecoder decoder;
        std::string request;

        // Handshake first: exactly one hello frame gates the session.
        if (detail::recvFrame(fd, decoder, request)) {
            bool accept = false;
            const std::string reply = processHandshake(request, accept);
            detail::sendFrame(fd, reply);
            if (accept) {
                while (running.load() && detail::recvFrame(fd, decoder, request)) {
                    if (!detail::sendFrame(fd, processRequest(session, request))) {
                        break;
                    }
                }
            }
        }
    }

    void handle(int fd) {
        try {
            serve(fd);
        } catch (...) {
            // This is the top of a connection thread: anything escaping here
            // would call std::terminate, so one bad connection would take the
            // host application down with it.
        }

        const std::lock_guard<std::mutex> lock(connMutex);
        connFds.erase(fd);
        ::close(fd);
        --activeHandlers;
        // Signalled while holding the lock: stop() may be waiting to tear this
        // object down the moment the count reaches zero, and it can only wake
        // once this thread's last access to it (the unlock below) is done.
        allHandlersDone.notify_all();
    }

    // Takes one of the bounded connection slots. False when the cap is reached
    // or the bookkeeping allocation failed; nothing is reserved in either case,
    // so the caller just closes the fd. The insert runs before the count is
    // bumped, so a throw from it leaves the bookkeeping consistent.
    bool reserveSlot(int fd) {
        try {
            const std::lock_guard<std::mutex> lock(connMutex);
            if (activeHandlers >= kMaxConnections) {
                return false;
            }
            connFds.insert(fd);
            ++activeHandlers;
            return true;
        } catch (...) {
            return false;
        }
    }

    // Starts the handler for an accepted connection. Returns false when the
    // connection cannot be served — over the cap, or the OS refused another
    // thread — so the caller just closes the fd; an exception escaping here
    // would reach the top of the accept thread and abort the process.
    bool spawnHandler(int fd) {
        if (!reserveSlot(fd)) {
            return false;
        }
        try {
            // Detached rather than collected: a joinable thread keeps its ~8 MB
            // stack mapped until it is joined, which leaked once per
            // connection. stop() waits on the count instead.
            std::thread([this, fd] { handle(fd); }).detach();
            return true;
        } catch (...) {
            // std::system_error when the OS refuses another thread, std::bad_alloc
            // when its state cannot be allocated: either way the slot goes back.
            const std::lock_guard<std::mutex> lock(connMutex);
            connFds.erase(fd);
            --activeHandlers;
            allHandlersDone.notify_all();
            return false;
        }
    }

    void acceptOnce() {
        pollfd fds[2]{};
        fds[0].fd = listenFd;
        fds[0].events = POLLIN;
        fds[1].fd = wakeReadFd;  // stop() writes here to end the wait immediately
        fds[1].events = POLLIN;
        const int ready = ::poll(fds, 2, 200);  // also wake periodically to re-check running
        if (ready <= 0 || !running.load() || (fds[0].revents & POLLIN) == 0) {
            return;
        }
        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        if (!spawnHandler(fd)) {
            ::close(fd);
        }
    }

    void acceptLoop() {
        while (running.load()) {
            try {
                acceptOnce();
            } catch (...) {
                // This is the top of the accept thread: anything escaping here
                // — a std::bad_alloc from the handler bookkeeping, say — would
                // call std::terminate and take the host application down. Drop
                // the connection instead and keep listening.
            }
        }
    }

    // Called only once the accept thread has been joined: until then it can
    // still name these descriptors, and closing one hands its number back to
    // the process while another thread is passing it to poll()/accept().
    void closeDescriptors() {
        for (int* fd : {&listenFd, &wakeReadFd, &wakeWriteFd}) {
            if (*fd >= 0) {
                ::close(*fd);
                *fd = -1;
            }
        }
    }
};

BridgeServer::BridgeServer(BridgeSession& session) : m(std::make_unique<Impl>(session)) {}

BridgeServer::~BridgeServer() { stop(); }

bool BridgeServer::start(std::uint16_t port) {
    if (m->running.load()) {
        return true;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local-only bind (spec)
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 16) != 0) {
        ::close(fd);
        return false;
    }

    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) {
        ::close(fd);
        return false;
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
        m->boundPort = ntohs(actual.sin_port);
    }

    m->listenFd = fd;
    m->wakeReadFd = wake[0];
    m->wakeWriteFd = wake[1];
    m->running.store(true);
    try {
        m->acceptThread = std::thread([this] { m->acceptLoop(); });
    } catch (...) {
        // No accept thread means nothing would ever be served: report the
        // failure instead of listening on a socket nobody reads.
        m->running.store(false);
        m->closeDescriptors();
        m->boundPort = 0;
        return false;
    }
    return true;
}

void BridgeServer::stop() {
    if (!m->running.exchange(false)) {
        return;
    }
    if (m->listenFd >= 0) {
        ::shutdown(m->listenFd, SHUT_RDWR);
    }
    if (m->wakeWriteFd >= 0) {
        const char byte = 0;
        const ssize_t written = ::write(m->wakeWriteFd, &byte, 1);
        // A failed wake only costs the accept thread one poll timeout: it
        // re-checks running on every pass.
        static_cast<void>(written);
    }
    if (m->acceptThread.joinable()) {
        m->acceptThread.join();
    }
    m->closeDescriptors();
    // Unblock any handler parked in recv, then wait for them all to finish.
    {
        std::unique_lock<std::mutex> lock(m->connMutex);
        for (const int fd : m->connFds) {
            ::shutdown(fd, SHUT_RDWR);
        }
        m->allHandlersDone.wait(lock, [this] { return m->activeHandlers == 0; });
    }
    m->connFds.clear();
    m->boundPort = 0;
}

bool BridgeServer::isListening() const { return m->running.load(); }

std::uint16_t BridgeServer::port() const { return m->boundPort; }

}  // namespace cyber::net
