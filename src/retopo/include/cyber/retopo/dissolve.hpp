#pragma once

#include <cstddef>
#include <vector>

#include "cyber/core/mesh.hpp"

// Edge dissolve and rotate operators (manual-retopology gesture grammar,
// app task 3.4):
//
//   * dissolveEdge — removes an interior edge and merges its two faces into
//     one (a scribbled-over triangle pair becomes a quad; a quad pair
//     becomes a hexagon).
//   * rotateEdgeAny — "circle over an edge" rotate: triangle pairs flip
//     their shared diagonal (Mesh::flipEdge); quad pairs are dissolved into
//     a hexagon and re-split one corner over, turning the loop-flow
//     direction of the pair.
//
// Both validate before mutating: a failed operation leaves the mesh
// unchanged.
namespace cyber::retopo {

namespace dissolve_detail {

// Combined boundary ring of the two faces of `edge`, starting at one edge
// endpoint: face A's cycle from b around to a (skipping the dissolved
// edge), then face B's cycle from a around to b. Empty when the merged
// ring would be degenerate (shared vertices beyond the edge endpoints).
inline std::vector<VertexId> mergedRing(const Mesh& mesh, EdgeId edge, FaceId fa, FaceId fb) {
    const auto [a, b] = mesh.edgeVertices(edge);
    const auto cycleFrom = [&](FaceId f, VertexId from, VertexId to) {
        // Face cycle rotated to start at `from`, truncated before `to`.
        std::vector<VertexId> cycle = mesh.faceVertices(f);
        std::vector<VertexId> out;
        const std::size_t n = cycle.size();
        std::size_t startIndex = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (cycle[i] == from) {
                startIndex = i;
                break;
            }
        }
        if (startIndex == n) {
            return out;
        }
        // The rotation must walk AWAY from the dissolved edge: pick the
        // cycle direction whose first step is not `to`.
        const bool forward = !(cycle[(startIndex + 1) % n] == to);
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t i = forward ? (startIndex + k) % n : (startIndex + n - k) % n;
            if (k > 0 && cycle[i] == to) {
                break;
            }
            out.push_back(cycle[i]);
        }
        return out;
    };

    const std::vector<VertexId> half0 = cycleFrom(fa, b, a);
    const std::vector<VertexId> half1 = cycleFrom(fb, a, b);
    std::vector<VertexId> ring;
    ring.reserve(half0.size() + half1.size());
    ring.insert(ring.end(), half0.begin(), half0.end());
    ring.insert(ring.end(), half1.begin(), half1.end());
    // Reject rings that repeat a vertex (faces sharing more than the one
    // edge): addFace would fold over.
    for (std::size_t i = 0; i < ring.size(); ++i) {
        for (std::size_t j = i + 1; j < ring.size(); ++j) {
            if (ring[i] == ring[j]) {
                return {};
            }
        }
    }
    return ring;
}

}  // namespace dissolve_detail

// Dissolves an interior edge: its two faces are replaced by one face over
// their combined boundary. Returns the merged face, or invalid (mesh
// unchanged) when `edge` is dead, a boundary edge, or the merge would be
// degenerate.
[[nodiscard]] inline FaceId dissolveEdge(Mesh& mesh, EdgeId edge) {
    if (!mesh.isAlive(edge) || mesh.edgeFaceCount(edge) != 2) {
        return FaceId{};
    }
    const std::vector<FaceId> faces = mesh.edgeFaces(edge);
    const std::vector<VertexId> ring = dissolve_detail::mergedRing(mesh, edge, faces[0], faces[1]);
    if (ring.size() < 3) {
        return FaceId{};
    }
    mesh.removeFace(faces[0]);
    mesh.removeFace(faces[1]);
    return mesh.addFace(ring);
}

// Rotates an interior edge shared by two triangles (diagonal flip) or two
// quads (dissolve + re-split one ring corner over). Returns true on
// success; false leaves the mesh unchanged.
[[nodiscard]] inline bool rotateEdgeAny(Mesh& mesh, EdgeId edge) {
    if (!mesh.isAlive(edge) || mesh.edgeFaceCount(edge) != 2) {
        return false;
    }
    const std::vector<FaceId> faces = mesh.edgeFaces(edge);
    const std::size_t s0 = mesh.faceSize(faces[0]);
    const std::size_t s1 = mesh.faceSize(faces[1]);
    if (s0 == 3 && s1 == 3) {
        return mesh.flipEdge(edge);
    }
    if (s0 != 4 || s1 != 4) {
        return false;
    }
    // Quad pair: hexagon ring starts at edge endpoint b (mergedRing
    // contract), with the old edge spanning ring[0] -> ring[3]. Rotating
    // one corner over splits between ring[1] and ring[4].
    const std::vector<VertexId> ring = dissolve_detail::mergedRing(mesh, edge, faces[0], faces[1]);
    if (ring.size() != 6) {
        return false;
    }
    if (mesh.edgeBetween(ring[1], ring[4]).valid()) {
        return false;  // rotated diagonal already exists elsewhere
    }
    mesh.removeFace(faces[0]);
    mesh.removeFace(faces[1]);
    const FaceId merged = mesh.addFace(ring);
    if (!merged.valid()) {
        return false;
    }
    return mesh.splitFace(merged, ring[1], ring[4]).valid();
}

}  // namespace cyber::retopo
