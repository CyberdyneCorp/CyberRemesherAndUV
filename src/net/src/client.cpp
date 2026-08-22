#include "cyber/net/client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <exception>
#include <json.hpp>
#include <utility>

#include "cyber/net/protocol.hpp"
#include "json_read.hpp"
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
        // dump() throws type_error.316 on a string that is not valid UTF-8 — a
        // Latin-1 label or message handed in by the host — out of an API that
        // reports errors as `false`. The server's reply path takes the same
        // replacing handler; valid UTF-8 dumps byte-identically.
        if (!detail::sendFrame(fd, request.dump(-1, ' ', false, json::error_handler_t::replace))) {
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
        const std::string type = exchange(request, response) ? replyType(response) : std::string{};
        return !type.empty() && type != "error";
    }

    // Sends `request` and, when the reply is of `expectedType`, hands it to
    // `decode`. The reply is peer-controlled and is read through accessors that
    // throw on a missing or retyped member, so the decode runs under a catch:
    // client.hpp documents a protocol error as a `false` return, and letting an
    // exception out of here would unwind into the host application instead.
    template <typename Decoder>
    bool decodeReply(const json& request, const char* expectedType, Decoder decode) {
        json response;
        if (!exchange(request, response) || replyType(response) != expectedType) {
            return false;
        }
        try {
            return decode(response);
        } catch (const std::exception&) {
            return false;
        }
    }

    // Reply "type", or empty when the reply is not an object with a string one.
    static std::string replyType(const json& response) {
        return detail::readString(response, "type", std::string{});
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
    if (!m->exchange(hello, response) || Impl::replyType(response) != "welcome") {
        m->handshakeError = detail::readString(response, "reason", "handshake failed");
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
    return m->decodeReply({{"type", "pull_target"}}, "target", [&](const json& response) {
        const bool hasTarget = response.value("present", false);
        WireMesh mesh = detail::meshFromJson(response.at("mesh"));
        present = hasTarget;
        out = std::move(mesh);
        return true;
    });
}

bool BridgeClient::pushEditMesh(const WireMesh& mesh) {
    return m->command({{"type", "push_editmesh"}, {"mesh", detail::meshToJson(mesh)}});
}

bool BridgeClient::pullEditMesh(WireMesh& out) {
    return m->decodeReply({{"type", "pull_editmesh"}}, "editmesh", [&](const json& response) {
        out = detail::meshFromJson(response.at("mesh"));
        return true;
    });
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
    std::vector<std::string> ids;
    if (!m->decodeReply({{"type", "poll_presses"}}, "presses", [&](const json& response) {
            ids = response.at("ids").get<std::vector<std::string>>();
            return true;
        })) {
        return {};
    }
    return ids;
}

bool BridgeClient::setSymmetry(const std::string& axis, bool enabled) {
    return m->command({{"type", "set_symmetry"}, {"axis", axis}, {"enabled", enabled}});
}

bool BridgeClient::querySymmetry(SymmetryState& out) {
    return m->decodeReply({{"type", "query_symmetry"}}, "symmetry", [&](const json& response) {
        out.axis = detail::readString(response, "axis", "none");
        out.enabled = response.value("enabled", false);
        return true;
    });
}

bool BridgeClient::queryChanged(std::uint64_t marker, bool& changed, std::uint64_t& revision) {
    return m->decodeReply(
        {{"type", "query_changed"}, {"marker", marker}}, "changed", [&](const json& response) {
            changed = response.value("changed", false);
            revision = detail::readUnsigned(response, "revision", std::uint64_t{0});
            return true;
        });
}

bool BridgeClient::setCamera(const CameraPose& pose) {
    json p{{"position", {pose.position[0], pose.position[1], pose.position[2]}},
           {"target", {pose.target[0], pose.target[1], pose.target[2]}},
           {"up", {pose.up[0], pose.up[1], pose.up[2]}},
           {"fov", pose.fovDegrees}};
    return m->command({{"type", "set_camera"}, {"pose", p}});
}

bool BridgeClient::getCamera(CameraPose& out) {
    return m->decodeReply({{"type", "get_camera"}}, "camera", [&](const json& response) {
        const json& p = response.at("pose");
        CameraPose pose;
        for (std::size_t i = 0; i < 3; ++i) {
            pose.position[i] = p.at("position").at(i).get<float>();
            pose.target[i] = p.at("target").at(i).get<float>();
            pose.up[i] = p.at("up").at(i).get<float>();
        }
        pose.fovDegrees = p.value("fov", 45.0f);
        out = pose;
        return true;
    });
}

}  // namespace cyber::net
