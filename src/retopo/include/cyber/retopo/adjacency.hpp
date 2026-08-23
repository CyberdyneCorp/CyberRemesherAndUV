#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Flat (CSR) one-ring and vertex->face adjacency over a cyber::Mesh.
//
// The brush actions (Relax, the soft-selection sweeps) visit every vertex's
// neighbourhood once per sweep. Asking the mesh for it hands back a fresh
// std::vector each time, so an interactive drag frame spends most of its budget
// in the allocator. This table answers the same two queries from flat arrays
// built ONCE per topology change and reused by every sweep after it.
//
// The stored orders are exactly the ones oneRing() and Mesh::vertexFaces()
// produce — neighbours in the vertex's incident-edge insertion order, incident
// faces ascending by face id — so swapping a mesh query for a lookup here is
// arithmetically inert: the same values are summed in the same order.
//
// The table is a SNAPSHOT. Its owner rebuilds it after anything that adds,
// removes or rewires an element; positions may move freely underneath it.
namespace cyber::retopo {

class MeshAdjacency {
public:
    // Rebuilds the table for `mesh`. Dead vertices get empty ranges.
    void build(const Mesh& mesh) {
        const std::size_t vertices = mesh.vertexCapacity();
        buildRings(mesh, vertices);
        buildFaces(mesh, vertices);
        m_vertexCapacity = vertices;
        m_built = true;
    }

    void clear() {
        m_ringBegin.clear();
        m_ring.clear();
        m_faceBegin.clear();
        m_faces.clear();
        m_vertexCapacity = 0;
        m_built = false;
    }

    [[nodiscard]] bool built() const { return m_built; }

    // Cheap bounds guard for callers handed a table they did not build: a
    // mismatch means the table certainly does not describe `mesh`. It is NOT a
    // freshness proof (a flip rewires without resizing) — the owner is still
    // responsible for rebuilding after a topology edit.
    [[nodiscard]] bool covers(const Mesh& mesh) const {
        return m_built && m_vertexCapacity == mesh.vertexCapacity();
    }

    [[nodiscard]] std::span<const VertexId> ring(VertexId v) const {
        return slice(m_ring, m_ringBegin, v);
    }

    [[nodiscard]] std::span<const FaceId> faces(VertexId v) const {
        return slice(m_faces, m_faceBegin, v);
    }

private:
    template <typename T>
    [[nodiscard]] static std::span<const T> slice(const std::vector<T>& values,
                                                  const std::vector<std::size_t>& begin,
                                                  VertexId v) {
        const std::size_t i = static_cast<std::size_t>(v.value);
        if (i + 1 >= begin.size()) {
            return {};
        }
        return std::span<const T>(values.data() + begin[i], begin[i + 1] - begin[i]);
    }

    // One entry per incident edge, in the vertex's edge insertion order — the
    // sequence oneRing() returns, duplicates and all.
    void buildRings(const Mesh& mesh, std::size_t vertices) {
        m_ringBegin.assign(vertices + 1, 0);
        for (std::size_t i = 0; i < vertices; ++i) {
            const VertexId v{static_cast<Index>(i)};
            m_ringBegin[i + 1] = mesh.isAlive(v) ? mesh.vertexEdges(v).size() : 0;
        }
        prefixSum(m_ringBegin);
        m_ring.assign(m_ringBegin.back(), VertexId{});
        for (std::size_t i = 0; i < vertices; ++i) {
            const VertexId v{static_cast<Index>(i)};
            if (!mesh.isAlive(v)) {
                continue;
            }
            std::size_t at = m_ringBegin[i];
            for (const EdgeId e : mesh.vertexEdges(v)) {
                const auto [a, b] = mesh.edgeVertices(e);
                m_ring[at++] = (a == v) ? b : a;
            }
        }
    }

    // Faces are walked in id order and appended to each of their corners, so
    // every vertex's list comes out ascending by face id — the order
    // Mesh::vertexFaces sorts into. A corner repeated within one face would be
    // listed once there, so the equal-to-previous guard drops it here too.
    void buildFaces(const Mesh& mesh, std::size_t vertices) {
        m_faceBegin.assign(vertices + 1, 0);
        countFaceCorners(mesh);
        prefixSum(m_faceBegin);
        m_faces.assign(m_faceBegin.back(), FaceId{});
        std::vector<std::size_t> at(m_faceBegin.begin(), m_faceBegin.end() - 1);
        forEachFaceCorner(mesh, [&](Index vertex, FaceId face) {
            std::size_t& next = at[vertex];
            if (next > m_faceBegin[vertex] && m_faces[next - 1] == face) {
                return;  // same face twice on one corner
            }
            m_faces[next++] = face;
        });
        // The duplicate guard can leave a corner's slot short; close the gaps
        // so the ranges stay contiguous.
        compactFaces(at, vertices);
    }

    void countFaceCorners(const Mesh& mesh) {
        forEachFaceCorner(mesh, [&](Index vertex, FaceId) { ++m_faceBegin[vertex + 1]; });
    }

    template <typename Visit>
    static void forEachFaceCorner(const Mesh& mesh, Visit visit) {
        for (Index f = 0; f < mesh.faceCapacity(); ++f) {
            const FaceId face{f};
            if (!mesh.isAlive(face)) {
                continue;
            }
            for (const VertexId v : mesh.faceVertices(face)) {
                visit(v.value, face);
            }
        }
    }

    // Shifts each vertex's face range down over the holes the duplicate guard
    // left, updating the offsets to match.
    void compactFaces(const std::vector<std::size_t>& used, std::size_t vertices) {
        std::size_t out = 0;
        for (std::size_t i = 0; i < vertices; ++i) {
            const std::size_t from = m_faceBegin[i];
            const std::size_t count = used[i] - from;
            m_faceBegin[i] = out;
            for (std::size_t k = 0; k < count; ++k) {
                m_faces[out + k] = m_faces[from + k];
            }
            out += count;
        }
        m_faceBegin[vertices] = out;
        m_faces.resize(out);
    }

    // Turns per-slot counts held in [1..n] into start offsets in [0..n].
    static void prefixSum(std::vector<std::size_t>& offsets) {
        for (std::size_t i = 1; i < offsets.size(); ++i) {
            offsets[i] += offsets[i - 1];
        }
    }

    std::vector<std::size_t> m_ringBegin;  // vertexCapacity + 1 offsets into m_ring
    std::vector<VertexId> m_ring;
    std::vector<std::size_t> m_faceBegin;  // vertexCapacity + 1 offsets into m_faces
    std::vector<FaceId> m_faces;
    std::size_t m_vertexCapacity = 0;
    bool m_built = false;
};

// ---- neighbourhood reductions over a prepared ring / face list -------------
//
// These are the bodies of neighborCentroid() and vertexNormal() with the ring
// handed in instead of gathered, so the caller can reuse one gather for both.

// Centroid of `ring`; `fallback` (the vertex's own position) when it is empty.
[[nodiscard]] inline Vec3 ringCentroid(const Mesh& mesh, std::span<const VertexId> ring,
                                       Vec3 fallback) {
    if (ring.empty()) {
        return fallback;
    }
    Vec3 sum{};
    for (const VertexId n : ring) {
        sum += mesh.position(n);
    }
    return sum * (1.0f / static_cast<float>(ring.size()));
}

namespace detail {

// Memo of the face normals one sweep reads. A vertex normal is the mean of its
// incident face normals, so a sweep asks each face for its normal once per
// corner — four times over for a quad. The relax sweep is Jacobi (every read of
// an iteration happens before any write of it), so within one iteration the
// answer cannot change and computing it once is exact, not approximate.
class FaceNormalCache {
public:
    explicit FaceNormalCache(const Mesh& mesh) : m_mesh(mesh) {}

    // Discards the memo; call at the start of every iteration, because the
    // previous one moved the vertices the normals are built from.
    void reset() {
        m_normals.assign(m_mesh.faceCapacity(), Vec3{});
        m_known.assign(m_mesh.faceCapacity(), 0);
    }

    [[nodiscard]] Vec3 normal(FaceId f) {
        const std::size_t i = static_cast<std::size_t>(f.value);
        if (i >= m_known.size()) {
            return m_mesh.faceNormal(f);
        }
        if (m_known[i] == 0) {
            m_normals[i] = m_mesh.faceNormal(f);
            m_known[i] = 1;
        }
        return m_normals[i];
    }

private:
    const Mesh& m_mesh;
    std::vector<Vec3> m_normals;
    std::vector<unsigned char> m_known;
};

}  // namespace detail

// Mean of the incident face normals, normalized.
[[nodiscard]] inline Vec3 ringNormal(detail::FaceNormalCache& normals,
                                     std::span<const FaceId> faces) {
    Vec3 n{};
    for (const FaceId f : faces) {
        n += normals.normal(f);
    }
    return normalized(n);
}

namespace detail {

// Neighbourhood lookups for one sweep: served from a prebuilt table when the
// caller has one, otherwise gathered from the mesh. The gathering path reuses
// its scratch buffer across vertices, so it allocates a bounded amount per
// sweep rather than a vector per vertex.
class RingSource {
public:
    RingSource(const Mesh& mesh, const MeshAdjacency* adjacency)
        : m_mesh(mesh),
          m_adjacency(adjacency != nullptr && adjacency->covers(mesh) ? adjacency : nullptr) {}

    [[nodiscard]] std::span<const VertexId> ring(VertexId v) {
        if (m_adjacency != nullptr) {
            return m_adjacency->ring(v);
        }
        m_ring.clear();
        for (const EdgeId e : m_mesh.vertexEdges(v)) {
            const auto [a, b] = m_mesh.edgeVertices(e);
            m_ring.push_back((a == v) ? b : a);
        }
        return m_ring;
    }

    [[nodiscard]] std::span<const FaceId> faces(VertexId v) {
        if (m_adjacency != nullptr) {
            return m_adjacency->faces(v);
        }
        m_faces = m_mesh.vertexFaces(v);
        return m_faces;
    }

private:
    const Mesh& m_mesh;
    const MeshAdjacency* m_adjacency;
    std::vector<VertexId> m_ring;
    std::vector<FaceId> m_faces;
};

}  // namespace detail

}  // namespace cyber::retopo
