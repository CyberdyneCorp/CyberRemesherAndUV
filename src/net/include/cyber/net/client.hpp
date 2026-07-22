#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cyber/net/session.hpp"

namespace cyber::net {

// Reference C++ client for the bridge protocol. Mirrors the Python client and
// backs the in-process integration tests. Every call is a synchronous
// request/response; a false return means a transport or protocol error.
class BridgeClient {
public:
    BridgeClient();
    ~BridgeClient();
    BridgeClient(const BridgeClient&) = delete;
    BridgeClient& operator=(const BridgeClient&) = delete;

    // Connects to host:port and performs the version handshake.
    bool connect(std::uint16_t port, const std::string& host = "127.0.0.1");
    [[nodiscard]] bool connected() const;
    [[nodiscard]] const std::string& handshakeError() const;
    void close();

    bool ping();
    bool pushTarget(const WireMesh& mesh);
    bool pullTarget(WireMesh& out, bool& present);
    bool pushEditMesh(const WireMesh& mesh);
    bool pullEditMesh(WireMesh& out);
    bool clearScene();
    bool closeDocument();
    bool showMessage(const std::string& text);
    bool addAction(const std::string& id, const std::string& label);
    bool removeAction(const std::string& id);
    bool pressAction(const std::string& id);
    std::vector<std::string> pollPresses();
    bool setSymmetry(const std::string& axis, bool enabled);
    bool querySymmetry(SymmetryState& out);
    bool queryChanged(std::uint64_t marker, bool& changed, std::uint64_t& revision);
    bool setCamera(const CameraPose& pose);
    bool getCamera(CameraPose& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace cyber::net
