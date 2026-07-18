#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"

// Component-link model (task 11.3). A bake often pairs specific pieces of the
// high-poly Target with specific pieces of the low-poly EditMesh - the classic
// example being a bolt modelled as a separate high-poly shell that should only
// project onto its matching low-poly bolt, never onto the plate behind it.
//
// A "component" is a connected face set (an island, `cyber::Mesh::islands()`).
// This model lets an artist draw explicit links from an EditMesh component to
// one or more Target components. When a component has no explicit link, source
// resolution falls back to the nearest Target surface. The per-component
// `bakeAlone` flag ("drawing-X" in the UI) marks a component to be baked in
// isolation.
namespace cyber::bakecage {

inline constexpr std::size_t kInvalidComponent = static_cast<std::size_t>(-1);

class ComponentLinks {
public:
    // Partitions both meshes into components and builds a BVH over the Target
    // for nearest-surface fallback. Both meshes are referenced only during
    // construction; the resulting links are plain indices.
    ComponentLinks(const Mesh& editMesh, const Mesh& targetMesh);

    [[nodiscard]] std::size_t editComponentCount() const { return m_editComponents.size(); }
    [[nodiscard]] std::size_t targetComponentCount() const { return m_targetComponents.size(); }

    // The faces of an EditMesh / Target component, in island order.
    [[nodiscard]] const std::vector<FaceId>& editComponentFaces(std::size_t component) const {
        return m_editComponents[component];
    }
    [[nodiscard]] const std::vector<FaceId>& targetComponentFaces(std::size_t component) const {
        return m_targetComponents[component];
    }

    // The EditMesh component a face belongs to, or kInvalidComponent.
    [[nodiscard]] std::size_t editComponentOfFace(FaceId face) const;
    // The Target component a face belongs to, or kInvalidComponent.
    [[nodiscard]] std::size_t targetComponentOfFace(FaceId face) const;

    // --- explicit links ----------------------------------------------------

    // Links an EditMesh component to a Target component (idempotent; links are
    // kept sorted and de-duplicated). Out-of-range indices are ignored.
    void link(std::size_t editComponent, std::size_t targetComponent);
    void unlink(std::size_t editComponent, std::size_t targetComponent);
    void clearLinks(std::size_t editComponent);
    [[nodiscard]] const std::vector<std::size_t>& links(std::size_t editComponent) const {
        return m_links[editComponent];
    }
    [[nodiscard]] bool hasExplicitLink(std::size_t editComponent) const {
        return !m_links[editComponent].empty();
    }

    // --- bake-alone ("drawing-X") flag ------------------------------------

    void setBakeAlone(std::size_t editComponent, bool value);
    [[nodiscard]] bool bakeAlone(std::size_t editComponent) const {
        return m_bakeAlone[editComponent] != 0;
    }

    // --- source resolution -------------------------------------------------

    struct Resolution {
        // Target components this EditMesh component should sample from.
        std::vector<std::size_t> targets;
        // true if `targets` came from explicit links, false if it is the
        // nearest-surface fallback.
        bool explicitLink = false;
    };

    // Resolves the Target source(s) for an EditMesh component: its explicit
    // links if any, otherwise the single nearest Target component (by closest
    // point from the component centroid). Returns an empty target list only
    // when the Target mesh has no faces.
    [[nodiscard]] Resolution resolveSource(std::size_t editComponent) const;

private:
    std::vector<std::vector<FaceId>> m_editComponents;
    std::vector<std::vector<FaceId>> m_targetComponents;
    std::vector<std::size_t> m_editFaceComponent;    // by FaceId, kInvalidComponent if none
    std::vector<std::size_t> m_targetFaceComponent;  // by FaceId
    std::vector<Vec3> m_editCentroids;               // per edit component
    std::vector<std::vector<std::size_t>> m_links;   // per edit component, sorted
    std::vector<std::uint8_t> m_bakeAlone;           // per edit component
    Bvh m_targetBvh;
};

}  // namespace cyber::bakecage
