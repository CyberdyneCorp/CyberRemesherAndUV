#pragma once

#include <optional>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Target/EditMesh snapping (manual-retopology spec, "Target and EditMesh scene
// model"): EditMesh vertices continuously snap to the Target surface via the
// accelerated closest-point path, an optional modifier snaps to the nearest
// Target *vertex* instead, and a flat plane models a loaded 2D image target.
namespace cyber::retopo {

// An oriented plane (point + unit normal). Models both the flat image-snapping
// target and the symmetry plane so the two share one projection path.
struct Plane {
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

// Signed distance from `p` to the plane (positive on the normal side).
[[nodiscard]] inline float signedDistance(const Plane& plane, Vec3 p) {
    return dot(p - plane.point, normalized(plane.normal));
}

// Orthogonal projection of `p` onto the plane.
[[nodiscard]] inline Vec3 projectToPlane(const Plane& plane, Vec3 p) {
    const Vec3 n = normalized(plane.normal);
    return p - n * dot(p - plane.point, n);
}

// Reflection of `p` across the plane.
[[nodiscard]] inline Vec3 mirrorAcrossPlane(const Plane& plane, Vec3 p) {
    const Vec3 n = normalized(plane.normal);
    return p - n * (2.0f * dot(p - plane.point, n));
}

// Result of snapping to the Target surface.
struct SurfaceHit {
    Vec3 point;
    FaceId face;
    float distanceSquared = 0.0f;
};

// Result of the vertex-snap modifier.
struct VertexHit {
    VertexId vertex;
    Vec3 point;
    float distanceSquared = 0.0f;
};

// A snapshot snapper over a Target mesh: builds a BVH for closest-surface
// queries and keeps a flat vertex table for the vertex-snap modifier. Rebuild
// after the Target changes (the BVH is a snapshot, mesh-core spec).
class SurfaceSnapper {
public:
    SurfaceSnapper() = default;
    explicit SurfaceSnapper(const Mesh& target);

    [[nodiscard]] bool empty() const { return m_bvh.empty(); }

    // Closest point on the Target surface to `query`.
    [[nodiscard]] SurfaceHit snapToSurface(Vec3 query) const;

    // Nearest Target vertex within `radius`; nullopt if none is close enough.
    [[nodiscard]] std::optional<VertexHit> snapToVertex(Vec3 query, float radius) const;

    // The underlying Target BVH, for callers that also need raycast picking
    // (viewport tap→surface queries) without building a second hierarchy.
    [[nodiscard]] const Bvh& bvh() const { return m_bvh; }

private:
    struct VertexRecord {
        VertexId id;
        Vec3 position;
    };
    Bvh m_bvh;
    std::vector<VertexRecord> m_vertices;
};

}  // namespace cyber::retopo
