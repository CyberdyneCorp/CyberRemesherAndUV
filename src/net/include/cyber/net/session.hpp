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

// A flow guide as it travels over the wire: an ordered polyline (flat xyz
// triples) plus its strength and influence radius. The bridge's job is
// transport + storage — the remesh itself is not a bridge command today, so
// guides drawn in a DCC land here and the engine reads them from the session.
struct WireGuide {
    std::vector<float> points;  // 3 floats per point, at least 2 points
    float strength = 1.0f;
    float radius = 0.0f;
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

    // Guidance transport: flow guides and painted density authored against the
    // current Target. Cleared alongside it, since both index into its geometry.
    void setGuides(std::vector<WireGuide> guides);
    [[nodiscard]] std::vector<WireGuide> guides() const;
    void clearGuides();
    void setDensity(std::vector<float> density);
    [[nodiscard]] std::vector<float> density() const;

    void clearScene();     // drop target + editmesh + guidance, bump revision
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
    std::vector<WireGuide> m_guides;
    std::vector<float> m_density;
    WireMesh m_editMesh;
    std::uint64_t m_editRevision = 0;
    std::string m_message;
    std::vector<ActionButton> m_actions;
    std::vector<std::string> m_presses;
    SymmetryState m_symmetry;
    CameraPose m_camera;
};

}  // namespace cyber::net
