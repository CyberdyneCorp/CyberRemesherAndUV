#include "cyber/render/mesh_stream.hpp"

#include <algorithm>
#include <cmath>

namespace cyber::render {

// ---- Aabb -------------------------------------------------------------------

void Aabb::expand(Vec3 p) {
    min = cyber::min(min, p);
    max = cyber::max(max, p);
}

void Aabb::expand(const Aabb& other) {
    if (other.empty()) {
        return;
    }
    min = cyber::min(min, other.min);
    max = cyber::max(max, other.max);
}

float Aabb::radius() const {
    if (empty()) {
        return 0.0f;
    }
    return length(extent()) * 0.5f;
}

// ---- Frustum ----------------------------------------------------------------

Frustum Frustum::fromViewProjection(const std::array<float, 16>& m) {
    // Column-major storage: m[col*4 + row]. Row r is (m[r], m[4+r], m[8+r], m[12+r]).
    auto row = [&](int r) {
        const auto ri = static_cast<std::size_t>(r);
        return Vec4{m[ri], m[4 + ri], m[8 + ri], m[12 + ri]};
    };
    const Vec4 r0 = row(0);
    const Vec4 r1 = row(1);
    const Vec4 r2 = row(2);
    const Vec4 r3 = row(3);

    Frustum frustum;
    const std::array<Vec4, 6> raw = {
        r3 + r0,  // left
        r3 - r0,  // right
        r3 + r1,  // bottom
        r3 - r1,  // top
        r3 + r2,  // near
        r3 - r2,  // far
    };
    for (std::size_t i = 0; i < 6; ++i) {
        Vec3 n{raw[i].x, raw[i].y, raw[i].z};
        const float len = length(n);
        if (len > 0.0f) {
            n = n / len;
            frustum.planes[i] = Plane{n, raw[i].w / len};
        } else {
            frustum.planes[i] = Plane{Vec3{0.0f, 0.0f, 0.0f}, raw[i].w};
        }
    }
    return frustum;
}

bool Frustum::intersects(const Aabb& box) const {
    if (box.empty()) {
        return false;
    }
    for (const Plane& plane : planes) {
        // Positive vertex: the box corner farthest along the plane normal.
        const Vec3 positive{
            plane.normal.x >= 0.0f ? box.max.x : box.min.x,
            plane.normal.y >= 0.0f ? box.max.y : box.min.y,
            plane.normal.z >= 0.0f ? box.max.z : box.min.z,
        };
        if (plane.signedDistance(positive) < 0.0f) {
            return false;  // wholly outside this plane
        }
    }
    return true;
}

// ---- MeshStreamManager ------------------------------------------------------

namespace {

// Clamps a coordinate into [0, cells) grid index space.
std::uint32_t cellIndex(float value, float lo, float invSpan, std::uint32_t cells) {
    if (cells == 0) {
        return 0;
    }
    const float t = (value - lo) * invSpan;  // ~[0,1]
    const float scaled = t * static_cast<float>(cells);
    const auto idx = static_cast<std::int64_t>(std::floor(scaled));
    const auto maxIdx = static_cast<std::int64_t>(cells) - 1;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(idx, 0, maxIdx));
}

}  // namespace

void MeshStreamManager::build(const Mesh& mesh, const StreamConfig& config) {
    m_config = config;
    m_config.gridResolution = std::max<std::uint32_t>(m_config.gridResolution, 1);
    m_config.lodCount = std::max<std::uint32_t>(m_config.lodCount, 1);

    m_bounds = Aabb{};
    m_chunks.clear();
    m_triangleIndices.clear();
    m_lodRanges.clear();
    m_totalTriangles = 0;

    // Overall bounds from live vertices.
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (mesh.isAlive(v)) {
            m_bounds.expand(mesh.position(v));
        }
    }
    if (m_bounds.empty()) {
        return;
    }

    // Uniform grid sized so the longest axis gets gridResolution cells.
    const Vec3 ext = m_bounds.extent();
    const float longest = std::max({ext.x, ext.y, ext.z, 1e-6f});
    const float cellSize = longest / static_cast<float>(m_config.gridResolution);
    const auto cellsAxis = [&](float axisExtent) {
        const auto n = static_cast<std::uint32_t>(std::ceil(axisExtent / cellSize));
        return std::max<std::uint32_t>(n, 1);
    };
    const std::uint32_t nx = cellsAxis(ext.x);
    const std::uint32_t ny = cellsAxis(ext.y);
    const std::uint32_t nz = cellsAxis(ext.z);
    const std::size_t cellCount = static_cast<std::size_t>(nx) * ny * nz;

    const Vec3 invSpan{
        ext.x > 0.0f ? 1.0f / ext.x : 0.0f,
        ext.y > 0.0f ? 1.0f / ext.y : 0.0f,
        ext.z > 0.0f ? 1.0f / ext.z : 0.0f,
    };

    // Bucket faces into cells by centroid.
    std::vector<std::vector<FaceId>> buckets(cellCount);
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const Vec3 c = mesh.faceCentroid(f);
        const std::uint32_t cx = cellIndex(c.x, m_bounds.min.x, invSpan.x, nx);
        const std::uint32_t cy = cellIndex(c.y, m_bounds.min.y, invSpan.y, ny);
        const std::uint32_t cz = cellIndex(c.z, m_bounds.min.z, invSpan.z, nz);
        const std::size_t bucket = (static_cast<std::size_t>(cz) * ny + cy) * nx + cx;
        buckets[bucket].push_back(f);
    }

    // Materialize non-empty buckets into chunks with flat triangle indices.
    for (std::vector<FaceId>& bucket : buckets) {
        if (bucket.empty()) {
            continue;
        }
        MeshChunk chunk;
        chunk.faces = std::move(bucket);
        chunk.triangleOffset = static_cast<std::uint32_t>(m_triangleIndices.size() / 3u);

        Vec3 centroidSum{0.0f, 0.0f, 0.0f};
        for (const FaceId f : chunk.faces) {
            const std::vector<VertexId> verts = mesh.faceVertices(f);
            for (const VertexId v : verts) {
                chunk.bounds.expand(mesh.position(v));
            }
            centroidSum += mesh.faceCentroid(f);
            // Fan-triangulate (v0, vi, vi+1).
            for (std::size_t i = 1; i + 1 < verts.size(); ++i) {
                m_triangleIndices.push_back(verts[0].value);
                m_triangleIndices.push_back(verts[i].value);
                m_triangleIndices.push_back(verts[i + 1].value);
            }
        }
        const auto triCount =
            static_cast<std::uint32_t>((m_triangleIndices.size() / 3u) - chunk.triangleOffset);
        chunk.triangleCount = triCount;
        chunk.centroid = centroidSum * (1.0f / static_cast<float>(chunk.faces.size()));
        m_totalTriangles += triCount;
        m_chunks.push_back(std::move(chunk));
    }

    // Per-chunk, per-LOD index ranges. LOD 0 is the full range; higher LODs
    // decimate by taking a strided subset of triangles (indexCount shrinks).
    m_lodRanges.resize(static_cast<std::size_t>(m_chunks.size()) * m_config.lodCount);
    for (std::size_t ci = 0; ci < m_chunks.size(); ++ci) {
        const MeshChunk& chunk = m_chunks[ci];
        const std::uint32_t baseIndex = chunk.triangleOffset * 3u;
        for (std::uint32_t lod = 0; lod < m_config.lodCount; ++lod) {
            const std::uint32_t stride = 1u << lod;  // 1, 2, 4, ...
            const std::uint32_t lodTris = std::max<std::uint32_t>(chunk.triangleCount / stride, 1);
            ChunkLod range;
            range.indexOffset = baseIndex;
            range.indexCount = std::min(lodTris, chunk.triangleCount) * 3u;
            m_lodRanges[ci * m_config.lodCount + lod] = range;
        }
    }
}

ChunkLod MeshStreamManager::lodRange(std::uint32_t chunkIndex, std::uint32_t lod) const {
    if (chunkIndex >= m_chunks.size() || m_config.lodCount == 0) {
        return ChunkLod{};
    }
    const std::uint32_t clampedLod = std::min(lod, m_config.lodCount - 1);
    return m_lodRanges[static_cast<std::size_t>(chunkIndex) * m_config.lodCount + clampedLod];
}

std::uint32_t MeshStreamManager::lodForDistance(float distance) const {
    if (m_config.lodStepDistance <= 0.0f) {
        return 0;
    }
    const float steps = distance / m_config.lodStepDistance;
    const auto lod = static_cast<std::uint32_t>(std::max(0.0f, std::floor(steps)));
    return std::min(lod, m_config.lodCount - 1);
}

std::vector<VisibleChunk> MeshStreamManager::selectVisible(const Frustum& frustum, Vec3 cameraPos,
                                                           SelectionStats* stats) const {
    std::vector<VisibleChunk> visible;
    visible.reserve(m_chunks.size());

    std::uint32_t culled = 0;
    for (std::size_t ci = 0; ci < m_chunks.size(); ++ci) {
        const MeshChunk& chunk = m_chunks[ci];
        if (!frustum.intersects(chunk.bounds)) {
            ++culled;
            continue;
        }
        VisibleChunk vc;
        vc.chunkIndex = static_cast<std::uint32_t>(ci);
        vc.distance = length(chunk.centroid - cameraPos);
        vc.lod = lodForDistance(vc.distance);
        vc.range = lodRange(vc.chunkIndex, vc.lod);
        visible.push_back(vc);
    }

    // Front-to-back so the budget keeps the nearest detail.
    std::sort(visible.begin(), visible.end(),
              [](const VisibleChunk& a, const VisibleChunk& b) { return a.distance < b.distance; });

    std::uint32_t dropped = 0;
    std::uint64_t triangles = 0;
    std::vector<VisibleChunk> kept;
    kept.reserve(visible.size());
    for (const VisibleChunk& vc : visible) {
        const std::uint64_t tris = vc.range.indexCount / 3u;
        if (triangles + tris > m_config.triangleBudget && !kept.empty()) {
            ++dropped;
            continue;
        }
        triangles += tris;
        kept.push_back(vc);
    }

    if (stats != nullptr) {
        stats->totalChunks = static_cast<std::uint32_t>(m_chunks.size());
        stats->visibleChunks = static_cast<std::uint32_t>(kept.size());
        stats->culledByFrustum = culled;
        stats->droppedByBudget = dropped;
        stats->selectedTriangles = triangles;
    }
    return kept;
}

}  // namespace cyber::render
