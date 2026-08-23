#pragma once

#include <cstddef>
#include <cstdint>
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

// Accounting for the ops that move vertices and re-project them onto the Target
// in the SAME pass (weighted transform, weighted relax).
//
// BOTH COUNTS ARE DISTINCT VERTICES, never per-iteration updates: a multi-
// iteration relax revisits the same vertex every sweep, so counting the writes
// would report a number larger than the mesh's vertex count. `moved` therefore
// never exceeds the live vertex count and `resnapped` never exceeds `moved`.
//
// `resnapped` counts the moved vertices whose re-projection correction was
// STRICTLY GREATER than the caller's epsilon. With the default epsilon of 0 a
// vertex the projection did not have to correct at all (correction exactly 0 —
// it already sat on the Target, which happens for weights so small the blended
// target is bit-identical to the current position) is not counted, so
// `moved - resnapped` is the number of vertices that were already on-surface.
struct ResnapReport {
    std::size_t moved = 0;
    std::size_t resnapped = 0;
    float maxSnapDistance = 0.0f;
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

    // Nearest Target vertex within `radius` (inclusive); nullopt if none is
    // close enough. Answered through a hierarchy over the Target's vertices, so
    // the cost does not grow with the Target — a 4M-triangle Target used to cost
    // a full scan per query, which no interactive frame budget survives.
    //
    // EXACT, not approximate: the pruning only discards subtrees that cannot
    // hold a closer vertex, and among vertices at exactly the same distance the
    // LOWEST VertexId wins, so the answer is the same one a scan in id order
    // returns, bit for bit.
    [[nodiscard]] std::optional<VertexHit> snapToVertex(Vec3 query, float radius) const;

    // The underlying Target BVH, for callers that also need raycast picking
    // (viewport tap→surface queries) without building a second hierarchy.
    [[nodiscard]] const Bvh& bvh() const { return m_bvh; }

    // Size of the vertex hierarchy snapToVertex answers from. Introspection for
    // the test that pins the query to a hierarchy: a scan over the vertex table
    // gives the same answers, so nothing else would notice it coming back.
    [[nodiscard]] std::size_t vertexNodeCount() const { return m_vertexNodes.size(); }

private:
    struct VertexRecord {
        VertexId id;
        Vec3 position;
    };
    // Node of the Target's vertex hierarchy. Leaves cover m_vertices[first,
    // first + count); internal nodes have their children at `first` and
    // `first + 1` of m_vertexNodes. Points, not triangles, so a median split on
    // the widest axis is well behaved and the bounds are exact.
    struct VertexNode {
        Vec3 boundsMin, boundsMax;
        std::uint32_t first = 0;
        std::uint32_t count = 0;  // 0 marks an internal node
    };

    void buildVertexNode(std::uint32_t node, std::uint32_t begin, std::uint32_t end);

    Bvh m_bvh;
    std::vector<VertexRecord> m_vertices;
    std::vector<VertexNode> m_vertexNodes;
};

}  // namespace cyber::retopo
