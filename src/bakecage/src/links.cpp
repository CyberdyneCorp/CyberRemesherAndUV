#include "cyber/bakecage/links.hpp"

#include <algorithm>

namespace cyber::bakecage {

namespace {

// Builds a face -> component map sized to the mesh's face capacity, so a face
// can be looked up by FaceId in O(1). Slots for dead faces stay invalid.
std::vector<std::size_t> buildFaceComponentMap(const Mesh& mesh,
                                               const std::vector<std::vector<FaceId>>& components) {
    std::vector<std::size_t> map(mesh.faceCapacity(), kInvalidComponent);
    for (std::size_t c = 0; c < components.size(); ++c) {
        for (const FaceId f : components[c]) {
            map[f.value] = c;
        }
    }
    return map;
}

Vec3 componentCentroid(const Mesh& mesh, const std::vector<FaceId>& faces) {
    if (faces.empty()) {
        return {};
    }
    Vec3 sum{};
    for (const FaceId f : faces) {
        sum += mesh.faceCentroid(f);
    }
    return sum / static_cast<float>(faces.size());
}

}  // namespace

ComponentLinks::ComponentLinks(const Mesh& editMesh, const Mesh& targetMesh)
    : m_editComponents(editMesh.islands()),
      m_targetComponents(targetMesh.islands()),
      m_editFaceComponent(buildFaceComponentMap(editMesh, m_editComponents)),
      m_targetFaceComponent(buildFaceComponentMap(targetMesh, m_targetComponents)),
      m_links(m_editComponents.size()),
      m_bakeAlone(m_editComponents.size(), std::uint8_t{0}),
      m_targetBvh(targetMesh) {
    m_editCentroids.reserve(m_editComponents.size());
    for (const auto& faces : m_editComponents) {
        m_editCentroids.push_back(componentCentroid(editMesh, faces));
    }
}

std::size_t ComponentLinks::editComponentOfFace(FaceId face) const {
    return face.value < m_editFaceComponent.size() ? m_editFaceComponent[face.value]
                                                   : kInvalidComponent;
}

std::size_t ComponentLinks::targetComponentOfFace(FaceId face) const {
    return face.value < m_targetFaceComponent.size() ? m_targetFaceComponent[face.value]
                                                     : kInvalidComponent;
}

void ComponentLinks::link(std::size_t editComponent, std::size_t targetComponent) {
    if (editComponent >= m_links.size() || targetComponent >= m_targetComponents.size()) {
        return;
    }
    auto& list = m_links[editComponent];
    const auto pos = std::lower_bound(list.begin(), list.end(), targetComponent);
    if (pos == list.end() || *pos != targetComponent) {
        list.insert(pos, targetComponent);
    }
}

void ComponentLinks::unlink(std::size_t editComponent, std::size_t targetComponent) {
    if (editComponent >= m_links.size()) {
        return;
    }
    auto& list = m_links[editComponent];
    const auto pos = std::lower_bound(list.begin(), list.end(), targetComponent);
    if (pos != list.end() && *pos == targetComponent) {
        list.erase(pos);
    }
}

void ComponentLinks::clearLinks(std::size_t editComponent) {
    if (editComponent < m_links.size()) {
        m_links[editComponent].clear();
    }
}

void ComponentLinks::setBakeAlone(std::size_t editComponent, bool value) {
    if (editComponent < m_bakeAlone.size()) {
        m_bakeAlone[editComponent] = value ? std::uint8_t{1} : std::uint8_t{0};
    }
}

ComponentLinks::Resolution ComponentLinks::resolveSource(std::size_t editComponent) const {
    Resolution result;
    if (editComponent >= m_editComponents.size()) {
        return result;
    }
    if (!m_links[editComponent].empty()) {
        result.targets = m_links[editComponent];
        result.explicitLink = true;
        return result;
    }
    // Nearest-surface fallback: the Target component owning the face closest to
    // the EditMesh component centroid.
    if (m_targetBvh.empty()) {
        return result;
    }
    const Bvh::ClosestHit hit = m_targetBvh.closestPoint(m_editCentroids[editComponent]);
    const std::size_t component = targetComponentOfFace(hit.face);
    if (component != kInvalidComponent) {
        result.targets.push_back(component);
    }
    return result;
}

}  // namespace cyber::bakecage
