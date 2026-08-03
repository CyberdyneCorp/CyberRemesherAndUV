#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"

// Meshlet clustering for the mesh-shader Target render path
// (openspec add-meshlet-target-path, task 3.1).
//
// A meshlet is a small bounded cluster of triangles that a Metal object/mesh
// shader pair can emit in one threadgroup. Two things make that faster than the
// plain indexed-vertex path on a multi-million-triangle Target:
//
//   * VERTEX REUSE — a cluster's vertices are deduplicated into a local list, so
//     each is transformed once per cluster rather than relying on the hardware's
//     post-transform cache across an unbounded index stream.
//   * CLUSTER CULLING — a whole cluster can be rejected before rasterisation by
//     its bounding sphere (frustum) and its NORMAL CONE (backfacing). On a closed
//     mesh roughly half the clusters face away from any given camera.
//
// Measured motivation (iPad Air M3, 4.8M-triangle real asset), RENDER cost only:
// indexed 7.01 ms at 1280x960 and 7.34 ms at 2732x2048; meshlet 5.40 ms and
// 6.53 ms. 4.5x the pixels for a few percent of frame time, so the Target is
// geometry-bound rather than fill-bound — precisely the case these two mechanisms
// address — and the meshlet path is 11-23% faster on the same geometry.
//
// CORRECTED figures. Every earlier number here (14.64 / 16.54 ms) was measured with
// a 22 MB full-resolution pixel READBACK inside the timed window: the offscreen
// harness blitted the colour texture to a shared buffer in the SAME command buffer
// the GPU timestamps bracket, so the probe legitimately timed render + readback and
// it was reported as "frame time". No real frame performs that copy. The corrected
// numbers are ~2.3x lower, and they retire the claim that the indexed path missed
// the 60 fps budget on a real asset — it never did.
//
// Deliberately NO cluster LOD (no simplification, no level selection, so no
// crack-free-boundary problem). The measured gap was ~20-25%; LOD is a separate,
// much larger change and is only justified if this is not enough.
//
// Operates on the RENDER STREAMS (compacted positions + a flat triangle index
// buffer), not on `Mesh` topology, because those are exactly the buffers the
// renderer binds — so meshlet vertex indices address the same array the position
// and normal buffers use, with no second mapping to keep in sync.
namespace cyber::remesh {

/// Cluster size limits. The defaults (64 vertices / 126 triangles) are the
/// conventional choice: 126 keeps the local index triple count at 378, which
/// packs into a 128-primitive threadgroup while leaving room for the payload,
/// and 64 vertices keeps the local index type at 8 bits.
struct MeshletCaps {
    std::uint32_t maxVertices = 64;
    std::uint32_t maxTriangles = 126;
};

/// One cluster. `vertexOffset`/`triangleOffset` index into `MeshletSet`'s shared
/// arrays, so the whole set is three flat buffers the GPU can bind directly.
struct Meshlet {
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t triangleOffset = 0;  // in TRIANGLES, not indices
    std::uint32_t triangleCount = 0;

    /// Bounding sphere, for frustum rejection.
    Vec3 center{};
    float radius = 0.0f;

    /// Normal cone for backface rejection: `axis` is the average triangle
    /// normal, `cutoff` the minimum dot(axis, n_i) over the cluster — so every
    /// triangle normal lies within acos(cutoff) of the axis.
    ///
    /// `cutoff <= 0` means the cluster's normals span more than a hemisphere and
    /// it must NEVER be backface-culled. Culling something visible leaves a hole
    /// in the model, so the degenerate case has to fail safe rather than clever.
    Vec3 coneAxis{0.0f, 0.0f, 1.0f};
    float coneCutoff = 0.0f;
};

/// A whole mesh's clusters plus their shared index arrays.
struct MeshletSet {
    std::vector<Meshlet> meshlets;
    /// Global vertex indices, grouped per meshlet.
    std::vector<std::uint32_t> vertices;
    /// Triangle corners as LOCAL offsets into the owning meshlet's slice of
    /// `vertices`; 3 per triangle. 8-bit because maxVertices <= 256.
    std::vector<std::uint8_t> indices;

    [[nodiscard]] bool empty() const { return meshlets.empty(); }
    /// Total triangles across all clusters — equals the input triangle count for
    /// a well-formed build, which is what the tests assert.
    [[nodiscard]] std::size_t triangleCount() const { return indices.size() / 3; }
};

/// Clusters `indices` (3 per triangle, into `positions`) into meshlets.
///
/// DETERMINISTIC: seeds from the lowest unassigned triangle and grows by the
/// candidate sharing the most vertices, breaking ties on the lower triangle
/// index. Same input, same output, on any platform — the render path's goldens
/// and the frame-time comparison both depend on that.
///
/// `positions` is xyz per vertex. A degenerate triangle (repeated corner) is
/// dropped rather than clustered; an out-of-range index makes the build return
/// an empty set, since a malformed stream must not produce a partially valid
/// one the renderer would draw.
[[nodiscard]] MeshletSet buildMeshlets(std::span<const float> positions,
                                       std::span<const std::uint32_t> indices,
                                       MeshletCaps caps = {});

}  // namespace cyber::remesh
