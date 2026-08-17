#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

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
    // Identity of this snapshot, unique for the process: every Bvh::flatten()
    // stamps a fresh value. A device backend that keeps the last snapshot
    // resident (see the accel backend contract) needs an exact identity, and
    // the array addresses are not one — a freed FlatBvh's storage comes back
    // from the allocator at the same address, with the same counts, for the
    // next mesh. 0 marks a snapshot not produced by flatten(), which no
    // backend may treat as resident.
    std::uint64_t serial = 0;
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
    // Cancellable, progress-reporting build. A Target import at multi-million
    // triangles takes seconds, which is a blocking wait on device, so the build
    // follows the library's ProgressSink/CancelToken contract (remeshing-pipeline
    // spec, "Progress reporting and cooperative cancellation"): `progress` is
    // reported over the whole build and `cancel` is polled at bounded intervals.
    // A cancelled build leaves an EMPTY Bvh — every query answers as it does for
    // an empty mesh — and sets cancelled(), so a caller can tell "nothing to
    // build" from "you asked me to stop".
    Bvh(const Mesh& mesh, ProgressSink* progress, const CancelToken* cancel);

    [[nodiscard]] bool empty() const { return m_triangles.empty(); }
    [[nodiscard]] std::size_t triangleCount() const { return m_triangles.size(); }
    // True when the constructor's CancelToken tripped before the tree was done.
    [[nodiscard]] bool cancelled() const { return m_cancelled; }

    [[nodiscard]] ClosestHit closestPoint(Vec3 query) const;
    [[nodiscard]] std::optional<RayHit> raycast(Vec3 origin, Vec3 direction,
                                                float maxDistance = 1e30f) const;

    // Surface-area-heuristic cost of the built hierarchy: the expected number of
    // node visits plus triangle tests for a random ray, in units where an
    // internal node costs 1 and a triangle test costs 1. It is a pure function
    // of the tree's shape and bounds — no queries, no timing — which makes it
    // the one build-quality number a test can assert on without being flaky.
    // Lower is better; a degenerate build shows up as a large multiple of the
    // cost a well-built tree over the same triangles reaches.
    [[nodiscard]] double sahCost() const;

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

    // Hierarchy construction (binned SAH, parallel above a size threshold).
    // Defined in bvh.cpp; a nested type so it reaches Triangle and Node.
    struct Builder;

    void gatherTriangles(const Mesh& mesh);

    std::vector<Triangle> m_triangles;
    std::vector<Node> m_nodes;
    bool m_cancelled = false;
};

// Exact closest point on a single triangle (Ericson, Real-Time Collision
// Detection). Exposed for tests and for backend parity kernels.
[[nodiscard]] Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);

}  // namespace cyber
