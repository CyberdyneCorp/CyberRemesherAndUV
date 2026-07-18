#pragma once

#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Small shared adjacency helpers for the brush-based retopo actions (Relax,
// Move, Tweak). Kept header-only and inline; they wrap cyber::Mesh queries so
// the action headers stay self-contained.
namespace cyber::retopo {

// One-ring vertex neighbours of `v` (deduplicated, deterministic order).
[[nodiscard]] inline std::vector<VertexId> oneRing(const Mesh& mesh, VertexId v) {
    std::vector<VertexId> ring;
    for (const EdgeId e : mesh.vertexEdges(v)) {
        const auto [a, b] = mesh.edgeVertices(e);
        const VertexId other = (a == v) ? b : a;
        ring.push_back(other);
    }
    return ring;
}

// Area-weighted-ish vertex normal (mean of incident face normals).
[[nodiscard]] inline Vec3 vertexNormal(const Mesh& mesh, VertexId v) {
    Vec3 n{};
    for (const FaceId f : mesh.vertexFaces(v)) {
        n += mesh.faceNormal(f);
    }
    return normalized(n);
}

// Centroid of the one-ring neighbours; returns `v`'s own position if isolated.
[[nodiscard]] inline Vec3 neighborCentroid(const Mesh& mesh, VertexId v) {
    const std::vector<VertexId> ring = oneRing(mesh, v);
    if (ring.empty()) {
        return mesh.position(v);
    }
    Vec3 sum{};
    for (const VertexId n : ring) {
        sum += mesh.position(n);
    }
    return sum * (1.0f / static_cast<float>(ring.size()));
}

}  // namespace cyber::retopo
