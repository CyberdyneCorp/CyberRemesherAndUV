#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cyber/net/protocol.hpp"

namespace cyber::net {

struct CameraPose {
    float position[3] = {0, 0, 0};
    float target[3] = {0, 0, 0};
    float up[3] = {0, 1, 0};
    float fovDegrees = 45.0f;
};

struct SymmetryState {
    std::string axis = "none";  // "none" | "x" | "y" | "z"
    bool enabled = false;
};

struct ActionButton {
    std::string id;
    std::string label;
};

// In-memory backing store the bridge acts on: the current Target and EditMesh,
// remote action buttons and their pending presses, symmetry, camera pose, the
// last message shown, and an EditMesh revision counter for change queries.
//
// This stands in for the document/session layer (group 8) that does not exist
// yet; when it lands, the server dispatch is re-pointed at it. Every method is
// mutex-guarded so connection threads never race the (future) UI thread.
class BridgeSession {
public:
    void setTarget(WireMesh mesh);
    [[nodiscard]] bool hasTarget() const;
    [[nodiscard]] WireMesh target() const;

    void setEditMesh(WireMesh mesh);          // bumps the revision
    [[nodiscard]] WireMesh editMesh() const;  // pulled by clients
    [[nodiscard]] std::uint64_t editMeshRevision() const;

    void clearScene();     // drop target + editmesh, bump revision
    void closeDocument();  // clearScene + drop actions/message

    void showMessage(std::string text);
    [[nodiscard]] std::string lastMessage() const;

    void addAction(std::string id, std::string label);
    void removeAction(const std::string& id);
    [[nodiscard]] std::vector<ActionButton> actions() const;
    void pressAction(const std::string& id);               // user tap (or injected in tests)
    [[nodiscard]] std::vector<std::string> takePresses();  // drains the queue

    void setSymmetry(SymmetryState symmetry);
    [[nodiscard]] SymmetryState symmetry() const;

    void setCamera(CameraPose pose);
    [[nodiscard]] CameraPose camera() const;

private:
    mutable std::mutex m_mutex;
    std::optional<WireMesh> m_target;
    WireMesh m_editMesh;
    std::uint64_t m_editRevision = 0;
    std::string m_message;
    std::vector<ActionButton> m_actions;
    std::vector<std::string> m_presses;
    SymmetryState m_symmetry;
    CameraPose m_camera;
};

}  // namespace cyber::net
