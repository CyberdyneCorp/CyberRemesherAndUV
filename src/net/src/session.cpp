#include "cyber/net/session.hpp"

#include <algorithm>
#include <utility>

namespace cyber::net {

void BridgeSession::setTarget(WireMesh mesh) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_target = std::move(mesh);
}

bool BridgeSession::hasTarget() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_target.has_value();
}

WireMesh BridgeSession::target() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_target.value_or(WireMesh{});
}

void BridgeSession::setEditMesh(WireMesh mesh) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_editMesh = std::move(mesh);
    ++m_editRevision;
}

WireMesh BridgeSession::editMesh() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_editMesh;
}

std::uint64_t BridgeSession::editMeshRevision() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_editRevision;
}

void BridgeSession::clearScene() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_target.reset();
    m_editMesh = WireMesh{};
    ++m_editRevision;
}

void BridgeSession::closeDocument() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_target.reset();
    m_editMesh = WireMesh{};
    ++m_editRevision;
    m_actions.clear();
    m_presses.clear();
    m_message.clear();
}

void BridgeSession::showMessage(std::string text) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_message = std::move(text);
}

std::string BridgeSession::lastMessage() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_message;
}

void BridgeSession::addAction(std::string id, std::string label) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (ActionButton& action : m_actions) {
        if (action.id == id) {
            action.label = std::move(label);  // re-registering updates the label
            return;
        }
    }
    m_actions.push_back({std::move(id), std::move(label)});
}

void BridgeSession::removeAction(const std::string& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_actions.erase(std::remove_if(m_actions.begin(), m_actions.end(),
                                   [&id](const ActionButton& a) { return a.id == id; }),
                    m_actions.end());
}

std::vector<ActionButton> BridgeSession::actions() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_actions;
}

void BridgeSession::pressAction(const std::string& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const bool known = std::any_of(m_actions.begin(), m_actions.end(),
                                   [&id](const ActionButton& a) { return a.id == id; });
    if (known) {
        m_presses.push_back(id);
    }
}

std::vector<std::string> BridgeSession::takePresses() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    out.swap(m_presses);
    return out;
}

void BridgeSession::setSymmetry(SymmetryState symmetry) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_symmetry = std::move(symmetry);
}

SymmetryState BridgeSession::symmetry() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_symmetry;
}

void BridgeSession::setCamera(CameraPose pose) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_camera = pose;
}

CameraPose BridgeSession::camera() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_camera;
}

}  // namespace cyber::net
