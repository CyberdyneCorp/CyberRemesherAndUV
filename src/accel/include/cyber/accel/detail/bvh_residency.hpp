#pragma once

#include <cstddef>
#include <cstdint>

#include "cyber/core/bvh.hpp"

// Identity of a flattened BVH as handed to IBackend::closestPointsBvh /
// raycastBvh, so a device backend can keep the last upload resident instead of
// repacking and re-uploading the whole hierarchy per call. The AO bake issues
// one query per texel, so a per-call upload makes bake cost O(texels x target
// triangles) — the same pathology the host side already hoisted out by
// flattening once per pass.
//
// The key is FlatBvh::serial — a process-wide counter stamped by Bvh::flatten()
// — plus the array addresses and sizes. The serial is what makes it exact:
// addresses and a content fingerprint are not enough, because a freed FlatBvh's
// storage comes back from the allocator at the same address with the same
// counts for the next mesh, and two different meshes can agree on any fixed
// sample of their contents (root bounds and the extreme triangles are pinned by
// the silhouette, so an interior edit changes neither). Serial 0 marks a
// snapshot that did not come from flatten(), which never counts as resident.
//
// Reading the key stays O(1) — it never scans the arrays, which would just move
// the per-call linear cost rather than remove it. The backend contract
// (backend.hpp) therefore still forbids mutating a snapshot's node/triangle
// arrays in place between queries; hand over a rebuilt snapshot instead, which
// carries a fresh serial.
namespace cyber::accel::detail {

struct BvhResidencyKey {
    std::uint64_t serial = 0;
    const FlatBvhNode* nodes = nullptr;
    std::size_t nodeCount = 0;
    const FlatBvhTri* tris = nullptr;
    std::size_t triCount = 0;

    [[nodiscard]] bool operator==(const BvhResidencyKey& other) const {
        return serial == other.serial && nodes == other.nodes && nodeCount == other.nodeCount &&
               tris == other.tris && triCount == other.triCount;
    }
    [[nodiscard]] bool operator!=(const BvhResidencyKey& other) const { return !(*this == other); }
};

// True when `query` names the very snapshot behind `uploaded`, so a backend may
// answer from what it already holds. An unidentified snapshot (serial 0) never
// matches, not even itself: without a serial there is nothing that separates it
// from the next snapshot to land on the same addresses.
[[nodiscard]] inline bool namesResidentBvh(const BvhResidencyKey& uploaded,
                                           const BvhResidencyKey& query) {
    return query.serial != 0 && uploaded == query;
}

[[nodiscard]] inline BvhResidencyKey makeBvhResidencyKey(const FlatBvh& flat) {
    BvhResidencyKey key;
    key.serial = flat.serial;
    key.nodes = flat.nodes.data();
    key.nodeCount = flat.nodes.size();
    key.tris = flat.tris.data();
    key.triCount = flat.tris.size();
    return key;
}

}  // namespace cyber::accel::detail
