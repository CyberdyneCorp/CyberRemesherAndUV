#include <dirent.h>
#include <doctest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "cyber/net/client.hpp"
#include "cyber/net/message.hpp"
#include "cyber/net/protocol.hpp"
#include "cyber/net/server.hpp"
#include "cyber/net/session.hpp"

namespace net = cyber::net;

namespace {

// Address space in kB from /proc/self/status, or 0 where it is unavailable
// (non-Linux POSIX). Used to observe per-connection leaks.
std::size_t virtualMemoryKb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmSize:") {
            std::size_t kb = 0;
            return (status >> kb) ? kb : 0;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

// Open descriptors of this process, or 0 where /proc is unavailable (non-Linux
// POSIX). Only the difference between two readings is meaningful.
std::size_t openFdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count;
}

// One-shot heap-failure injector for threads other than the one that arms it.
// The bridge's accept thread allocates where no test can reach — the connection
// bookkeeping and the handler thread's own state — so this is how an exhausted
// address space is reproduced there. Inert until armed, and it disarms itself
// on the first foreign allocation.
std::atomic<bool> g_failForeignAlloc{false};
std::atomic<bool> g_foreignAllocFailed{false};
std::thread::id g_armingThread{};

void armForeignAllocationFailure() {
    g_armingThread = std::this_thread::get_id();
    g_foreignAllocFailed.store(false, std::memory_order_relaxed);
    g_failForeignAlloc.store(true, std::memory_order_release);
}

void disarmForeignAllocationFailure() { g_failForeignAlloc.store(false, std::memory_order_release); }

bool foreignAllocationFailed() { return g_foreignAllocFailed.load(std::memory_order_acquire); }

// Minimal raw connection to the bridge. BridgeClient only ever sends
// well-formed requests, so putting arbitrary bytes on the wire needs its own
// socket. Reads are timeout-bounded so a regression fails instead of hanging.
class RawConnection {
public:
    explicit RawConnection(std::uint16_t port) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return;
        }
        timeval timeout{};
        timeout.tv_sec = 5;
        ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr),
                      static_cast<socklen_t>(sizeof(addr))) != 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }
    ~RawConnection() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    RawConnection(const RawConnection&) = delete;
    RawConnection& operator=(const RawConnection&) = delete;

    [[nodiscard]] bool ok() const { return m_fd >= 0; }

    bool send(const std::string& payload) {
        const std::string frame = net::encodeFrame(payload);
        return ::send(m_fd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
    }

    // One framed reply; empty on disconnect or read timeout.
    std::string receive() {
        std::string out;
        char buffer[512];
        while (!m_decoder.next(out)) {
            const ssize_t n = ::recv(m_fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return {};
            }
            m_decoder.feed(buffer, static_cast<std::size_t>(n));
        }
        return out;
    }

private:
    int m_fd = -1;
    net::FrameDecoder m_decoder;
};

net::WireMesh sampleMesh() {
    net::WireMesh m;
    m.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    m.faces = {{0, 1, 2}, {0, 2, 3}};
    m.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    m.colors = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1};
    return m;
}

}  // namespace

// Replaces the global allocator so the injector above can reach threads the
// tests do not own. Without it armed this is the ordinary allocation path. The
// size bound keeps the injection on the small bookkeeping allocations it is
// aimed at, well away from any buffer another thread might take.
void* operator new(std::size_t size) {
    constexpr std::size_t kInjectableSize = 256;
    if (g_failForeignAlloc.load(std::memory_order_acquire) && size <= kInjectableSize &&
        std::this_thread::get_id() != g_armingThread) {
        g_failForeignAlloc.store(false, std::memory_order_relaxed);
        g_foreignAllocFailed.store(true, std::memory_order_release);
        throw std::bad_alloc();
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

TEST_CASE("frame codec round-trips and reassembles split reads") {
    const std::string payload = R"({"type":"ping"})";
    const std::string frame = net::encodeFrame(payload);
    REQUIRE(frame.size() == payload.size() + 4);

    net::FrameDecoder decoder;
    // Feed one byte at a time to prove reassembly across partial reads.
    std::string out;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        decoder.feed(frame.data() + i, 1);
        if (i + 1 < frame.size()) {
            REQUIRE_FALSE(decoder.next(out));
        }
    }
    REQUIRE(decoder.next(out));
    REQUIRE(out == payload);
    REQUIRE_FALSE(decoder.next(out));
}

TEST_CASE("frame decoder rejects an oversized length without allocating it") {
    net::FrameDecoder decoder;
    // Length prefix far above the cap, no payload.
    const char header[4] = {'\x7f', '\x7f', '\x7f', '\x7f'};
    decoder.feed(header, 4);
    std::string out;
    REQUIRE_FALSE(decoder.next(out));
    REQUIRE(decoder.failed());
}

TEST_CASE("handshake accepts a matching version and rejects a mismatch") {
    bool accept = false;
    const std::string ok = net::processHandshake(R"({"type":"hello","protocol":1})", accept);
    REQUIRE(accept);
    REQUIRE(ok.find("welcome") != std::string::npos);

    accept = true;
    const std::string bad = net::processHandshake(R"({"type":"hello","protocol":99})", accept);
    REQUIRE_FALSE(accept);
    REQUIRE(bad.find("reject") != std::string::npos);
    REQUIRE(bad.find("99") != std::string::npos);  // reports the client version back
}

TEST_CASE("processRequest never throws on malformed or unknown input") {
    net::BridgeSession session;
    REQUIRE(net::processRequest(session, "not json").find("error") != std::string::npos);
    REQUIRE(net::processRequest(session, R"({"type":"nope"})").find("error") != std::string::npos);
    REQUIRE(net::processRequest(session, R"({"type":"ping"})").find("pong") != std::string::npos);

    // A byte that is not valid UTF-8 — a lone 0xFF, or a Latin-1 string field
    // from the DCC. The parse error quotes the offending input, so echoing
    // e.what() into the reply left invalid UTF-8 in it and dump() threw
    // type_error.316 out of a function documented never to throw.
    const std::string rawByte(1, static_cast<char>(0xff));
    REQUIRE(net::processRequest(session, rawByte).find("error") != std::string::npos);
    REQUIRE(net::processRequest(session, R"({"type":"message","text":")" + rawByte + R"("})")
                .find("error") != std::string::npos);
    REQUIRE(net::processRequest(session, R"({"type":"nope)" + rawByte + R"("})").find("error") !=
            std::string::npos);

    bool accept = true;
    REQUIRE(net::processHandshake(rawByte, accept).find("reject") != std::string::npos);
    REQUIRE_FALSE(accept);
}

TEST_CASE("server is off by default and reports no port") {
    net::BridgeSession session;
    net::BridgeServer server(session);
    REQUIRE_FALSE(server.isListening());
    REQUIRE(server.port() == 0);
}

TEST_CASE("bridge round-trips the full command set over a live local socket") {
    net::BridgeSession session;
    net::BridgeServer server(session);
    REQUIRE(server.start(0));  // ephemeral local port
    REQUIRE(server.isListening());
    REQUIRE(server.port() != 0);

    net::BridgeClient client;
    REQUIRE(client.connect(server.port()));
    REQUIRE(client.connected());

    SUBCASE("ping") { REQUIRE(client.ping()); }

    SUBCASE("target push / pull preserves geometry, UVs and colors") {
        const net::WireMesh mesh = sampleMesh();
        REQUIRE(client.pushTarget(mesh));
        REQUIRE(session.hasTarget());
        net::WireMesh pulled;
        bool present = false;
        REQUIRE(client.pullTarget(pulled, present));
        REQUIRE(present);
        REQUIRE(pulled == mesh);
    }

    SUBCASE("editmesh push / pull and change marker") {
        bool changed = false;
        std::uint64_t rev = 0;
        REQUIRE(client.queryChanged(0, changed, rev));
        REQUIRE_FALSE(changed);  // nothing pushed yet, marker 0 == revision 0

        const net::WireMesh mesh = sampleMesh();
        REQUIRE(client.pushEditMesh(mesh));
        net::WireMesh pulled;
        REQUIRE(client.pullEditMesh(pulled));
        REQUIRE(pulled == mesh);

        REQUIRE(client.queryChanged(rev, changed, rev));
        REQUIRE(changed);  // revision advanced past the old marker
    }

    SUBCASE("remote action registration, press and poll") {
        REQUIRE(client.addAction("bake", "Bake Now"));
        REQUIRE(client.pollPresses().empty());
        REQUIRE(client.pressAction("bake"));  // simulate the user's tap
        const std::vector<std::string> presses = client.pollPresses();
        REQUIRE(presses.size() == 1);
        REQUIRE(presses[0] == "bake");
        REQUIRE(client.pollPresses().empty());  // drained
    }

    SUBCASE("symmetry query reflects what was set") {
        REQUIRE(client.setSymmetry("x", true));
        net::SymmetryState sym;
        REQUIRE(client.querySymmetry(sym));
        REQUIRE(sym.axis == "x");
        REQUIRE(sym.enabled);
    }

    SUBCASE("camera pose streams round-trip") {
        net::CameraPose pose;
        pose.position[0] = 1.5f;
        pose.target[2] = -3.0f;
        pose.fovDegrees = 60.0f;
        REQUIRE(client.setCamera(pose));
        net::CameraPose got;
        REQUIRE(client.getCamera(got));
        REQUIRE(got.position[0] == doctest::Approx(1.5f));
        REQUIRE(got.target[2] == doctest::Approx(-3.0f));
        REQUIRE(got.fovDegrees == doctest::Approx(60.0f));
    }

    SUBCASE("message and scene lifecycle") {
        REQUIRE(client.showMessage("hello from client"));
        REQUIRE(session.lastMessage() == "hello from client");
        REQUIRE(client.pushTarget(sampleMesh()));
        REQUIRE(client.clearScene());
        REQUIRE_FALSE(session.hasTarget());
    }

    client.close();
    server.stop();
    REQUIRE_FALSE(server.isListening());
}

TEST_CASE("a client speaking the wrong version is refused") {
    net::BridgeSession session;
    net::BridgeServer server(session);
    REQUIRE(server.start(0));
    // The reference client always speaks the current version, so exercise the
    // rejection path through the pure handshake (the live wire path for the
    // matching version is covered above).
    bool accept = true;
    const std::string reply = net::processHandshake(R"({"type":"hello","protocol":2})", accept);
    REQUIRE_FALSE(accept);
    REQUIRE(reply.find("reject") != std::string::npos);
    server.stop();
}

TEST_CASE("a non-UTF-8 payload on the wire is answered, not fatal to the server") {
    net::BridgeSession session;
    net::BridgeServer server(session);
    REQUIRE(server.start(0));

    RawConnection conn(server.port());
    REQUIRE(conn.ok());
    REQUIRE(conn.send(R"({"type":"hello","protocol":1})"));
    REQUIRE(conn.receive().find("welcome") != std::string::npos);

    // The reply used to carry this byte back, and the throw from dump() had
    // nothing to catch it on the handler thread: the whole host process died.
    const std::string rawByte(1, static_cast<char>(0xff));
    REQUIRE(conn.send(R"({"type":"message","text":")" + rawByte + R"("})"));
    REQUIRE(conn.receive().find("error") != std::string::npos);

    // Both the connection and the server survive it.
    REQUIRE(conn.send(R"({"type":"ping"})"));
    REQUIRE(conn.receive().find("pong") != std::string::npos);

    net::BridgeClient client;
    REQUIRE(client.connect(server.port()));
    REQUIRE(client.ping());
    client.close();
    server.stop();
}

TEST_CASE("sequential connections do not accumulate handler threads") {
    net::BridgeSession session;
    net::BridgeServer server(session);
    REQUIRE(server.start(0));

    // First connection pays one-off allocations that are not per-connection.
    {
        net::BridgeClient warmup;
        REQUIRE(warmup.connect(server.port()));
        REQUIRE(warmup.ping());
        warmup.close();
    }

    const std::size_t before = virtualMemoryKb();
    constexpr int kConnections = 128;
    for (int i = 0; i < kConnections; ++i) {
        net::BridgeClient client;
        REQUIRE(client.connect(server.port()));
        REQUIRE(client.ping());
        client.close();
    }

    if (before != 0) {
        // Handler threads were kept joinable until stop(), so each connection
        // pinned its ~8 MB stack: these 128 grew the address space by ~1 GB.
        // Reaped handlers hand their stacks back for reuse.
        CHECK(virtualMemoryKb() < before + std::size_t{128} * 1024u);  // 128 MB of slack
    }
    server.stop();
}

TEST_CASE("stop() joins the accept thread and releases every descriptor it owned") {
    net::BridgeSession session;
    net::BridgeServer server(session);

    // A first cycle pays one-off allocations and descriptors that are not
    // per-cycle, so the counts below are read after it.
    REQUIRE(server.start(0));
    server.stop();

    const std::size_t fdsBefore = openFdCount();
    const auto begin = std::chrono::steady_clock::now();
    constexpr int kCycles = 24;
    for (int i = 0; i < kCycles; ++i) {
        REQUIRE(server.start(0));
        const std::uint16_t port = server.port();
        REQUIRE(port != 0);
        {
            net::BridgeClient client;
            REQUIRE(client.connect(port));
            REQUIRE(client.ping());
            client.close();
        }
        server.stop();
        REQUIRE_FALSE(server.isListening());
        // The listening socket is closed, not merely unattended.
        RawConnection refused(port);
        CHECK_FALSE(refused.ok());
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - begin)
                               .count();

    if (fdsBefore != 0) {
        // The listening socket used to be closed while the accept thread could
        // still name it — a data race, and the number went back to the process
        // mid-use. It now outlives that thread, so it (and the wake pipe that
        // ends the thread's wait) must still be released on every cycle.
        CHECK(openFdCount() == fdsBefore);
    }
    // Bounded so a shutdown that hangs on the join, or that waits out the
    // accept loop's poll timeout on every cycle, fails here instead of
    // stalling CI.
    CHECK(elapsedMs < 3000);
}

TEST_CASE("an allocation failure on the accept thread drops the connection, not the process") {
    net::BridgeSession session;
    net::BridgeServer server(session);
    REQUIRE(server.start(0));

    // Fail the accept thread's next allocation — the connection bookkeeping, or
    // the handler thread's state. Uncaught, it escaped the top of the accept
    // thread and std::terminate took the whole host process down with it.
    armForeignAllocationFailure();
    RawConnection doomed(server.port());
    for (int i = 0; i < 2000 && !foreignAllocationFailed(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    disarmForeignAllocationFailure();
    REQUIRE(doomed.ok());
    REQUIRE(foreignAllocationFailed());  // bounded wait: the injection did fire

    // That one connection is dropped and the server keeps serving.
    CHECK(doomed.receive().empty());
    REQUIRE(server.isListening());
    net::BridgeClient client;
    REQUIRE(client.connect(server.port()));
    REQUIRE(client.ping());
    client.close();
    server.stop();
}

TEST_CASE("guidance transport: guides and density round-trip and clear with the scene") {
    net::BridgeSession session;

    SUBCASE("push_guides -> pull_guides") {
        const std::string push = net::processRequest(
            session, R"({"type":"push_guides","guides":[{"points":[[0,0,0],[1,0,0],[1,1,0]],)"
                     R"("strength":0.5,"radius":0.25}]})");
        REQUIRE(push.find("\"ok\"") != std::string::npos);
        REQUIRE(session.guides().size() == 1);
        REQUIRE(session.guides()[0].points.size() == 9);
        REQUIRE(session.guides()[0].strength == doctest::Approx(0.5f));

        const std::string pull = net::processRequest(session, R"({"type":"pull_guides"})");
        REQUIRE(pull.find("\"guides\"") != std::string::npos);
        REQUIRE(pull.find("0.25") != std::string::npos);
    }

    SUBCASE("clear_guides drops them") {
        (void)net::processRequest(
            session,
            R"({"type":"push_guides","guides":[{"points":[[0,0,0],[1,0,0]],"radius":0.1}]})");
        REQUIRE(session.guides().size() == 1);
        REQUIRE(net::processRequest(session, R"({"type":"clear_guides"})").find("\"ok\"") !=
                std::string::npos);
        REQUIRE(session.guides().empty());
    }

    SUBCASE("push_density -> pull_density") {
        REQUIRE(net::processRequest(session, R"({"type":"push_density","values":[1.0,4.0,0.5]})")
                    .find("\"ok\"") != std::string::npos);
        REQUIRE(session.density().size() == 3);
        REQUIRE(net::processRequest(session, R"({"type":"pull_density"})").find("4.0") !=
                std::string::npos);
    }

    SUBCASE("a malformed guide payload is an error reply, not a crash") {
        // "points" missing entirely.
        REQUIRE(net::processRequest(session, R"({"type":"push_guides","guides":[{"radius":1}]})")
                    .find("error") != std::string::npos);
        REQUIRE(session.guides().empty());
    }

    SUBCASE("a short guide point is rejected on its shape, not read out of bounds") {
        // A point with fewer than three components used to reach nlohmann's
        // unchecked const operator[], which indexes the underlying vector past
        // its end. The reply must be the shape rejection, so requiring the
        // exact message is what pins the check: whatever the out-of-range read
        // happened to produce could never name the point shape.
        const std::string shape = "each point must be";
        const char* const malformed[] = {
            R"({"type":"push_guides","guides":[{"points":[[1.0]],"radius":0.1}]})",
            R"({"type":"push_guides","guides":[{"points":[[0,0,0],[1,0]],"radius":0.1}]})",
            R"({"type":"push_guides","guides":[{"points":[3.0],"radius":0.1}]})",
        };
        for (const char* const request : malformed) {
            const std::string reply = net::processRequest(session, request);
            CHECK(reply.find("error") != std::string::npos);
            CHECK(reply.find(shape) != std::string::npos);
            CHECK(session.guides().empty());
        }

        // A well-formed payload still goes through untouched.
        REQUIRE(net::processRequest(
                    session,
                    R"({"type":"push_guides","guides":[{"points":[[0,0,0],[1,0,0]],"radius":0.1}]})")
                    .find("\"ok\"") != std::string::npos);
        CHECK(session.guides().size() == 1);
    }

    SUBCASE("clear_scene drops guidance with the Target it was drawn on") {
        (void)net::processRequest(
            session,
            R"({"type":"push_guides","guides":[{"points":[[0,0,0],[1,0,0]],"radius":0.1}]})");
        (void)net::processRequest(session, R"({"type":"push_density","values":[1.0]})");
        REQUIRE(net::processRequest(session, R"({"type":"clear_scene"})").find("\"ok\"") !=
                std::string::npos);
        REQUIRE(session.guides().empty());
        REQUIRE(session.density().empty());
    }
}
