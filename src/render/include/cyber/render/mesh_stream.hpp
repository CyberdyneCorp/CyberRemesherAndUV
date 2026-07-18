#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// CPU-side level-of-detail and streaming manager for multi-million-triangle
// Targets (viewport-rendering spec, task 7.2). This layer is fully real,
// portable C++: it partitions a cyber::Mesh into spatial chunks, precomputes
// per-chunk bounds, and selects a visible, budgeted, LOD-resolved subset for a
// given camera. Actual GPU upload is the RHI's job; this decides *what* to send.
namespace cyber::render {

// Axis-aligned bounding box. An empty box has min > max on every axis.
struct Aabb {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};

    [[nodiscard]] bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }
    void expand(Vec3 p);
    void expand(const Aabb& other);
    [[nodiscard]] Vec3 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] Vec3 extent() const { return max - min; }
    [[nodiscard]] float radius() const;  // half of the diagonal length
};

// Half-space plane (dot(normal, x) + d = 0); positive side is the interior.
struct Plane {
    Vec3 normal{0.0f, 0.0f, 0.0f};
    float d = 0.0f;
    [[nodiscard]] float signedDistance(Vec3 p) const { return dot(normal, p) + d; }
};

// Six-plane view frustum. Planes point inward, so an AABB is culled when it
// lies entirely on the negative side of any plane.
struct Frustum {
    std::array<Plane, 6> planes{};

    // Extract inward-pointing, normalized planes from a row-major-multiply
    // view*projection matrix (column-major storage, Gribb & Hartmann).
    [[nodiscard]] static Frustum fromViewProjection(const std::array<float, 16>& viewProj);
    // Conservative test: true if the box is not entirely outside the frustum.
    [[nodiscard]] bool intersects(const Aabb& box) const;
};

// One spatial partition of the source mesh. `faces` indexes into the mesh; the
// triangle range references this manager's flat triangle-index buffer.
struct MeshChunk {
    Aabb bounds{};
    std::vector<FaceId> faces;
    std::uint32_t triangleOffset = 0;  // into MeshStreamManager::triangleIndices()
    std::uint32_t triangleCount = 0;
    Vec3 centroid{0.0f, 0.0f, 0.0f};
};

// Per-LOD triangle-index ranges into the flat buffer. LOD 0 is full detail;
// higher LODs are decimated strides for distant chunks.
struct ChunkLod {
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
};

struct StreamConfig {
    // Target chunk count along the longest axis of the mesh bounds.
    std::uint32_t gridResolution = 8;
    // Number of discrete LODs (>= 1). Distant chunks use coarser strides.
    std::uint32_t lodCount = 3;
    // World-space distance at which a chunk drops one LOD level.
    float lodStepDistance = 10.0f;
    // Upper bound on triangles selected per frame; farther chunks are dropped
    // once the budget is spent.
    std::uint64_t triangleBudget = 4'000'000;
};

struct VisibleChunk {
    std::uint32_t chunkIndex = 0;
    std::uint32_t lod = 0;
    float distance = 0.0f;
    ChunkLod range{};  // index range to draw at the chosen LOD
};

struct SelectionStats {
    std::uint32_t totalChunks = 0;
    std::uint32_t visibleChunks = 0;
    std::uint32_t culledByFrustum = 0;
    std::uint32_t droppedByBudget = 0;
    std::uint64_t selectedTriangles = 0;
};

class MeshStreamManager {
public:
    MeshStreamManager() = default;

    // Partitions `mesh` into chunks and precomputes bounds, triangle indices and
    // per-chunk LOD ranges. n-gons are fan-triangulated. Deterministic.
    void build(const Mesh& mesh, const StreamConfig& config = {});

    [[nodiscard]] const StreamConfig& config() const { return m_config; }
    [[nodiscard]] const Aabb& bounds() const { return m_bounds; }
    [[nodiscard]] const std::vector<MeshChunk>& chunks() const { return m_chunks; }
    // Flat triangle-corner vertex indices for LOD 0 (3 per triangle), grouped by
    // chunk. Higher LODs reference sub-strides of the same buffer.
    [[nodiscard]] const std::vector<Index>& triangleIndices() const { return m_triangleIndices; }
    [[nodiscard]] std::uint64_t totalTriangles() const { return m_totalTriangles; }

    // LOD range for a chunk at a given level (clamped to [0, lodCount)).
    [[nodiscard]] ChunkLod lodRange(std::uint32_t chunkIndex, std::uint32_t lod) const;

    // Selects visible chunks for a camera. Chunks are frustum-culled, sorted
    // front-to-back, LOD-picked by distance and cut off at the triangle budget.
    [[nodiscard]] std::vector<VisibleChunk> selectVisible(const Frustum& frustum, Vec3 cameraPos,
                                                          SelectionStats* stats = nullptr) const;

private:
    [[nodiscard]] std::uint32_t lodForDistance(float distance) const;

    StreamConfig m_config{};
    Aabb m_bounds{};
    std::vector<MeshChunk> m_chunks;
    std::vector<Index> m_triangleIndices;
    // Per-chunk, per-LOD ranges laid out as chunkIndex * lodCount + lod.
    std::vector<ChunkLod> m_lodRanges;
    std::uint64_t m_totalTriangles = 0;
};

}  // namespace cyber::render
