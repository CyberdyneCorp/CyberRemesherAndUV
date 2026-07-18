#pragma once

#include <json.hpp>

#include "cyber/net/protocol.hpp"

// Shared WireMesh <-> JSON conversion, used by both the server dispatch and
// the C++ client so the encoding is defined in exactly one place.
namespace cyber::net::detail {

inline nlohmann::json meshToJson(const WireMesh& mesh) {
    nlohmann::json j;
    j["positions"] = mesh.positions;
    j["faces"] = mesh.faces;
    if (!mesh.uvs.empty()) {
        j["uvs"] = mesh.uvs;
    }
    if (!mesh.colors.empty()) {
        j["colors"] = mesh.colors;
    }
    return j;
}

inline WireMesh meshFromJson(const nlohmann::json& j) {
    WireMesh mesh;
    if (j.contains("positions")) {
        mesh.positions = j.at("positions").get<std::vector<float>>();
    }
    if (j.contains("faces")) {
        mesh.faces = j.at("faces").get<std::vector<std::vector<std::uint32_t>>>();
    }
    if (j.contains("uvs")) {
        mesh.uvs = j.at("uvs").get<std::vector<float>>();
    }
    if (j.contains("colors")) {
        mesh.colors = j.at("colors").get<std::vector<float>>();
    }
    return mesh;
}

}  // namespace cyber::net::detail
