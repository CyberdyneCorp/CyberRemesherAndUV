#include "cyber/net/message.hpp"

#include <exception>

#include <json.hpp>

#include "mesh_json.hpp"

namespace cyber::net {

namespace {

using nlohmann::json;
using detail::meshFromJson;
using detail::meshToJson;

json error(const std::string& message) {
    return json{{"type", "error"}, {"message", message}};
}

json handle(BridgeSession& session, const json& req) {
    const std::string type = req.at("type").get<std::string>();

    if (type == "ping") {
        return json{{"type", "pong"}};
    }
    if (type == "push_target") {
        WireMesh mesh = meshFromJson(req.at("mesh"));
        const std::size_t v = mesh.vertexCount();
        const std::size_t f = mesh.faces.size();
        session.setTarget(std::move(mesh));
        return json{{"type", "ok"}, {"vertices", v}, {"faces", f}};
    }
    if (type == "push_editmesh") {
        session.setEditMesh(meshFromJson(req.at("mesh")));
        return json{{"type", "ok"}, {"revision", session.editMeshRevision()}};
    }
    if (type == "pull_editmesh") {
        return json{{"type", "editmesh"}, {"mesh", meshToJson(session.editMesh())}};
    }
    if (type == "pull_target") {
        return json{{"type", "target"},
                    {"present", session.hasTarget()},
                    {"mesh", meshToJson(session.target())}};
    }
    if (type == "clear_scene") {
        session.clearScene();
        return json{{"type", "ok"}};
    }
    if (type == "close_document") {
        session.closeDocument();
        return json{{"type", "ok"}};
    }
    if (type == "message") {
        session.showMessage(req.at("text").get<std::string>());
        return json{{"type", "ok"}};
    }
    if (type == "add_action") {
        session.addAction(req.at("id").get<std::string>(), req.value("label", std::string{}));
        return json{{"type", "ok"}};
    }
    if (type == "remove_action") {
        session.removeAction(req.at("id").get<std::string>());
        return json{{"type", "ok"}};
    }
    if (type == "press_action") {
        session.pressAction(req.at("id").get<std::string>());
        return json{{"type", "ok"}};
    }
    if (type == "poll_presses") {
        return json{{"type", "presses"}, {"ids", session.takePresses()}};
    }
    if (type == "list_actions") {
        json actions = json::array();
        for (const ActionButton& a : session.actions()) {
            actions.push_back({{"id", a.id}, {"label", a.label}});
        }
        return json{{"type", "actions"}, {"actions", actions}};
    }
    if (type == "set_symmetry") {
        SymmetryState sym;
        sym.axis = req.value("axis", std::string{"none"});
        sym.enabled = req.value("enabled", false);
        session.setSymmetry(sym);
        return json{{"type", "ok"}};
    }
    if (type == "query_symmetry") {
        const SymmetryState sym = session.symmetry();
        return json{{"type", "symmetry"}, {"axis", sym.axis}, {"enabled", sym.enabled}};
    }
    if (type == "query_changed") {
        const std::uint64_t marker = req.value("marker", std::uint64_t{0});
        const std::uint64_t rev = session.editMeshRevision();
        return json{{"type", "changed"}, {"changed", rev != marker}, {"revision", rev}};
    }
    if (type == "set_camera") {
        const json& p = req.at("pose");
        CameraPose pose;
        for (std::size_t i = 0; i < 3; ++i) {
            pose.position[i] = p.at("position").at(i).get<float>();
            pose.target[i] = p.at("target").at(i).get<float>();
            pose.up[i] = p.at("up").at(i).get<float>();
        }
        pose.fovDegrees = p.value("fov", 45.0f);
        session.setCamera(pose);
        return json{{"type", "ok"}};
    }
    if (type == "get_camera") {
        const CameraPose c = session.camera();
        return json{{"type", "camera"},
                    {"pose",
                     {{"position", {c.position[0], c.position[1], c.position[2]}},
                      {"target", {c.target[0], c.target[1], c.target[2]}},
                      {"up", {c.up[0], c.up[1], c.up[2]}},
                      {"fov", c.fovDegrees}}}};
    }
    return error("unknown command: " + type);
}

}  // namespace

std::string processHandshake(const std::string& helloJson, bool& accept) {
    accept = false;
    try {
        const json j = json::parse(helloJson);
        if (j.value("type", std::string{}) != "hello") {
            return json{{"type", "reject"},
                        {"reason", "expected hello"},
                        {"serverProtocol", kProtocolVersion}}
                .dump();
        }
        const std::uint32_t clientProtocol = j.value("protocol", std::uint32_t{0});
        if (clientProtocol != kProtocolVersion) {
            return json{{"type", "reject"},
                        {"reason", "incompatible protocol version"},
                        {"serverProtocol", kProtocolVersion},
                        {"clientProtocol", clientProtocol}}
                .dump();
        }
        accept = true;
        return json{{"type", "welcome"}, {"protocol", kProtocolVersion}, {"app", "CyberRemesher"}}
            .dump();
    } catch (const std::exception& e) {
        return json{{"type", "reject"},
                    {"reason", std::string("bad handshake: ") + e.what()},
                    {"serverProtocol", kProtocolVersion}}
            .dump();
    }
}

std::string processRequest(BridgeSession& session, const std::string& requestJson) {
    try {
        return handle(session, json::parse(requestJson)).dump();
    } catch (const std::exception& e) {
        return error(std::string("bad request: ") + e.what()).dump();
    }
}

}  // namespace cyber::net
