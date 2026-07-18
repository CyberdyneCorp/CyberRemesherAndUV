#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

namespace cyber {

// Flat, device-uploadable image of a Bvh (roadmap 4.6/5.8/11.1): plain-old-data
// arrays a GPU backend can memcpy verbatim, so closest-point and raycast can run
// on-device. Produced by Bvh::flatten(); core stays free of any accel/GPU
// dependency (the accel layer depends on core, never the reverse).
//
// Node encoding mirrors the internal Bvh::Node: triCount > 0 marks a leaf whose
// triangles are tris[leftFirst .. leftFirst+triCount); triCount == 0 marks an
// internal node whose two children are nodes[leftFirst] and nodes[leftFirst+1].
struct FlatBvhNode {
    float boundsMin[3];
    float boundsMax[3];
    std::uint32_t leftFirst;
    std::uint32_t triCount;
};

struct FlatBvhTri {
    float a[3];
    float b[3];
    float c[3];
    std::uint32_t face;  // owning FaceId::value
};

struct FlatBvh {
    std::vector<FlatBvhNode> nodes;
    std::vector<FlatBvhTri> tris;
};

// Bounding volume hierarchy over the triangles of a mesh (n-gons are
// fan-triangulated internally; hits report the owning FaceId). Supports the
// closest-point and raycast queries consumed by surface projection, retopo
// snapping and baking (mesh-core spec, "Spatial acceleration structure").
//
// The BVH is a snapshot: rebuild after the source mesh changes.
class Bvh {
public:
    struct ClosestHit {
        Vec3 point;
        float distanceSquared = 0.0f;
        FaceId face;
    };
    struct RayHit {
        Vec3 point;
        float t = 0.0f;  // distance along the (normalized) ray direction
        FaceId face;
    };

    Bvh() = default;
    explicit Bvh(const Mesh& mesh);

    [[nodiscard]] bool empty() const { return m_triangles.empty(); }
    [[nodiscard]] std::size_t triangleCount() const { return m_triangles.size(); }

    [[nodiscard]] ClosestHit closestPoint(Vec3 query) const;
    [[nodiscard]] std::optional<RayHit> raycast(Vec3 origin, Vec3 direction,
                                                float maxDistance = 1e30f) const;

    // Snapshot the hierarchy as flat POD arrays for device upload. The result
    // is self-contained (no back-references into the Bvh) and stays valid after
    // the Bvh is destroyed.
    [[nodiscard]] FlatBvh flatten() const;

private:
    struct Triangle {
        Vec3 a, b, c;
        FaceId face;
    };
    struct Node {
        Vec3 boundsMin, boundsMax;
        // Leaf: firstTriangle/triangleCount; internal: leftChild (+1 = right).
        std::uint32_t leftChild = 0;
        std::uint32_t firstTriangle = 0;
        std::uint32_t triangleCount = 0;
        [[nodiscard]] bool isLeaf() const { return triangleCount > 0; }
    };

    void build(std::uint32_t nodeIndex, std::uint32_t begin, std::uint32_t end);

    std::vector<Triangle> m_triangles;
    std::vector<Node> m_nodes;
};

// Exact closest point on a single triangle (Ericson, Real-Time Collision
// Detection). Exposed for tests and for backend parity kernels.
[[nodiscard]] Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);

}  // namespace cyber
