#pragma once

#include <array>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"

namespace cyber::remesh {

// Projection target for the isotropic stage. Wraps a BVH of the reference
// (source) surface and adds optional curved projection.
//
// When smoothNormalDegrees > 0 a projected point is evaluated on a Curved
// PN-triangle (Vlachos, Peters, Boyd & Mitchell 2001) reconstruction of the
// hit triangle instead of on its flat plane, so relaxed vertices follow the
// smooth surface rather than sinking onto coarse facets (remeshing-pipeline
// spec: "when smooth-normal degrees > 0 projection SHALL target a smoothed
// (PN-triangle or equivalent) surface").
//
// The angle is a crease threshold: incident face normals are averaged into
// per-corner normals only across edges whose dihedral angle is below it, so
// sharp features keep flat (face-normal) corners and their patches stay flat.
// smoothNormalDegrees == 0 degrades exactly to flat closest-point projection,
// making the smooth path opt-in and leaving default runs unchanged.
class ReferenceSurface {
public:
    ReferenceSurface() = default;
    ReferenceSurface(const Mesh& mesh, float smoothNormalDegrees);

    [[nodiscard]] bool empty() const { return m_bvh.empty(); }

    // Closest point on the reference surface, curved by the PN reconstruction
    // when smoothing is enabled and the hit face is a triangle.
    [[nodiscard]] Vec3 project(Vec3 query) const;

private:
    struct Patch {
        std::array<Vec3, 3> corner{};        // triangle corner positions
        std::array<Vec3, 3> cornerNormal{};  // crease-aware smoothed normals
        bool triangle = false;               // false -> flat closest-point fallback
    };

    Bvh m_bvh;
    bool m_smooth = false;
    std::vector<Patch> m_patches;  // indexed by FaceId::value; empty when !m_smooth
};

}  // namespace cyber::remesh
