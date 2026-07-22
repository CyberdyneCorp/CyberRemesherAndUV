#include "cyber/net/client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <exception>
#include <json.hpp>

#include "cyber/net/protocol.hpp"
#include "mesh_json.hpp"
#include "socket_io.hpp"

namespace cyber::net {

using nlohmann::json;

struct BridgeClient::Impl {
    int fd = -1;
    FrameDecoder decoder;
    bool connected = false;
    std::string handshakeError;

    // Sends `request` and parses the response frame into `response`.
    bool exchange(const json& request, json& response) {
        if (fd < 0) {
            return false;
        }
        if (!detail::sendFrame(fd, request.dump())) {
            return false;
        }
        std::string raw;
        if (!detail::recvFrame(fd, decoder, raw)) {
            return false;
        }
        try {
            response = json::parse(raw);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    // Convenience for commands whose success is just a non-error reply.
    bool command(const json& request) {
        json response;
        return exchange(request, response) && response.value("type", std::string{}) != "error";
    }
};

BridgeClient::BridgeClient() : m(std::make_unique<Impl>()) {}

BridgeClient::~BridgeClient() { close(); }

bool BridgeClient::connect(std::uint16_t port, const std::string& host) {
    close();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    m->fd = fd;

    // Handshake.
    json hello{{"type", "hello"}, {"protocol", kProtocolVersion}, {"client", "cyber-cpp"}};
    json response;
    if (!m->exchange(hello, response) || response.value("type", std::string{}) != "welcome") {
        m->handshakeError = response.contains("reason") ? response.at("reason").get<std::string>()
                                                        : "handshake failed";
        close();
        return false;
    }
    m->connected = true;
    return true;
}

bool BridgeClient::connected() const { return m->connected; }
const std::string& BridgeClient::handshakeError() const { return m->handshakeError; }

void BridgeClient::close() {
    if (m->fd >= 0) {
        ::close(m->fd);
        m->fd = -1;
    }
    m->connected = false;
    m->decoder = FrameDecoder{};
}

bool BridgeClient::ping() { return m->command({{"type", "ping"}}); }

bool BridgeClient::pushTarget(const WireMesh& mesh) {
    return m->command({{"type", "push_target"}, {"mesh", detail::meshToJson(mesh)}});
}

bool BridgeClient::pullTarget(WireMesh& out, bool& present) {
    json response;
    if (!m->exchange({{"type", "pull_target"}}, response) ||
        response.value("type", std::string{}) != "target") {
        return false;
    }
    present = response.value("present", false);
    out = detail::meshFromJson(response.at("mesh"));
    return true;
}

bool BridgeClient::pushEditMesh(const WireMesh& mesh) {
    return m->command({{"type", "push_editmesh"}, {"mesh", detail::meshToJson(mesh)}});
}

bool BridgeClient::pullEditMesh(WireMesh& out) {
    json response;
    if (!m->exchange({{"type", "pull_editmesh"}}, response) ||
        response.value("type", std::string{}) != "editmesh") {
        return false;
    }
    out = detail::meshFromJson(response.at("mesh"));
    return true;
}

bool BridgeClient::clearScene() { return m->command({{"type", "clear_scene"}}); }
bool BridgeClient::closeDocument() { return m->command({{"type", "close_document"}}); }

bool BridgeClient::showMessage(const std::string& text) {
    return m->command({{"type", "message"}, {"text", text}});
}

bool BridgeClient::addAction(const std::string& id, const std::string& label) {
    return m->command({{"type", "add_action"}, {"id", id}, {"label", label}});
}

bool BridgeClient::removeAction(const std::string& id) {
    return m->command({{"type", "remove_action"}, {"id", id}});
}

bool BridgeClient::pressAction(const std::string& id) {
    return m->command({{"type", "press_action"}, {"id", id}});
}

std::vector<std::string> BridgeClient::pollPresses() {
    json response;
    if (!m->exchange({{"type", "poll_presses"}}, response) ||
        response.value("type", std::string{}) != "presses") {
        return {};
    }
    return response.at("ids").get<std::vector<std::string>>();
}

bool BridgeClient::setSymmetry(const std::string& axis, bool enabled) {
    return m->command({{"type", "set_symmetry"}, {"axis", axis}, {"enabled", enabled}});
}

bool BridgeClient::querySymmetry(SymmetryState& out) {
    json response;
    if (!m->exchange({{"type", "query_symmetry"}}, response) ||
        response.value("type", std::string{}) != "symmetry") {
        return false;
    }
    out.axis = response.value("axis", std::string{"none"});
    out.enabled = response.value("enabled", false);
    return true;
}

bool BridgeClient::queryChanged(std::uint64_t marker, bool& changed, std::uint64_t& revision) {
    json response;
    if (!m->exchange({{"type", "query_changed"}, {"marker", marker}}, response) ||
        response.value("type", std::string{}) != "changed") {
        return false;
    }
    changed = response.value("changed", false);
    revision = response.value("revision", std::uint64_t{0});
    return true;
}

bool BridgeClient::setCamera(const CameraPose& pose) {
    json p{{"position", {pose.position[0], pose.position[1], pose.position[2]}},
           {"target", {pose.target[0], pose.target[1], pose.target[2]}},
           {"up", {pose.up[0], pose.up[1], pose.up[2]}},
           {"fov", pose.fovDegrees}};
    return m->command({{"type", "set_camera"}, {"pose", p}});
}

bool BridgeClient::getCamera(CameraPose& out) {
    json response;
    if (!m->exchange({{"type", "get_camera"}}, response) ||
        response.value("type", std::string{}) != "camera") {
        return false;
    }
    const json& p = response.at("pose");
    for (int i = 0; i < 3; ++i) {
        out.position[i] = p.at("position").at(static_cast<std::size_t>(i)).get<float>();
        out.target[i] = p.at("target").at(static_cast<std::size_t>(i)).get<float>();
        out.up[i] = p.at("up").at(static_cast<std::size_t>(i)).get<float>();
    }
    out.fovDegrees = p.value("fov", 45.0f);
    return true;
}

}  // namespace cyber::net
