#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>

namespace cyber::net {

class BridgeSession;

// Local-only, opt-in bridge server (network-bridge spec). It is off until
// start() is called, binds to 127.0.0.1 exclusively (never a routable
// interface, never a cloud relay), and serves each connection on its own
// thread so a stalled or malicious client cannot block the accept loop or the
// (future) UI thread. isListening() backs the required visible indicator.
class BridgeServer {
public:
    explicit BridgeServer(BridgeSession& session);
    ~BridgeServer();
    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    // Binds 127.0.0.1:port (port 0 selects an ephemeral port) and starts
    // serving. Returns false if the socket could not be bound.
    bool start(std::uint16_t port = 0);
    void stop();

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] std::uint16_t port() const;  // the actual bound port

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace cyber::net
