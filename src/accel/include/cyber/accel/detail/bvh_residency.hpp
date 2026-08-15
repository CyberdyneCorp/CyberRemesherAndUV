#pragma once

#include <cstddef>
#include <cstring>

#include "cyber/core/bvh.hpp"

// Identity of a flattened BVH as handed to IBackend::closestPointsBvh /
// raycastBvh, so a device backend can keep the last upload resident instead of
// repacking and re-uploading the whole hierarchy per call. The AO bake issues
// one query per texel, so a per-call upload makes bake cost O(texels x target
// triangles) — the same pathology the host side already hoisted out by
// flattening once per pass.
//
// The key is the array addresses and sizes plus a small content fingerprint
// (root bounds, first and last triangle). Addresses alone are not enough: a
// freed FlatBvh's storage can be reused at the same address with the same
// counts. The fingerprint is O(1) to read — it never scans the arrays, which
// would just move the per-call linear cost rather than remove it — so it
// catches a recycled allocation but is not a proof of equality. The backend
// contract (backend.hpp) therefore forbids mutating node/triangle arrays in
// place between queries.
namespace cyber::accel::detail {

struct BvhResidencyKey {
    const FlatBvhNode* nodes = nullptr;
    std::size_t nodeCount = 0;
    const FlatBvhTri* tris = nullptr;
    std::size_t triCount = 0;
    float rootBounds[6] = {};
    float firstTri[9] = {};
    float lastTri[9] = {};

    [[nodiscard]] bool operator==(const BvhResidencyKey& other) const {
        return nodes == other.nodes && nodeCount == other.nodeCount && tris == other.tris &&
               triCount == other.triCount &&
               std::memcmp(rootBounds, other.rootBounds, sizeof(rootBounds)) == 0 &&
               std::memcmp(firstTri, other.firstTri, sizeof(firstTri)) == 0 &&
               std::memcmp(lastTri, other.lastTri, sizeof(lastTri)) == 0;
    }
    [[nodiscard]] bool operator!=(const BvhResidencyKey& other) const { return !(*this == other); }
};

inline void copyTri(const FlatBvhTri& tri, float (&out)[9]) {
    std::memcpy(out, tri.a, sizeof(float) * 3);
    std::memcpy(out + 3, tri.b, sizeof(float) * 3);
    std::memcpy(out + 6, tri.c, sizeof(float) * 3);
}

[[nodiscard]] inline BvhResidencyKey makeBvhResidencyKey(const FlatBvhNode* nodes,
                                                         std::size_t nodeCount,
                                                         const FlatBvhTri* tris,
                                                         std::size_t triCount) {
    BvhResidencyKey key;
    key.nodes = nodes;
    key.nodeCount = nodeCount;
    key.tris = tris;
    key.triCount = triCount;
    if (nodes != nullptr && nodeCount > 0) {
        std::memcpy(key.rootBounds, nodes[0].boundsMin, sizeof(float) * 3);
        std::memcpy(key.rootBounds + 3, nodes[0].boundsMax, sizeof(float) * 3);
    }
    if (tris != nullptr && triCount > 0) {
        copyTri(tris[0], key.firstTri);
        copyTri(tris[triCount - 1], key.lastTri);
    }
    return key;
}

}  // namespace cyber::accel::detail
