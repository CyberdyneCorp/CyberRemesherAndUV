#pragma once

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <vector>

#include "cyber/core/mesh.hpp"

// Seam editing on the 3D mesh (uv-editing spec, "Seam editing on the 3D
// mesh"). A seam is a set of tagged edges; islands are the face-connected
// components you get when edge crossings through seam edges are forbidden.
namespace cyber::uv {

// A mutable set of seam edges. Marking, erasing and sewing map directly onto
// the Pencil / Erase gestures: draw creates a seam, erase deletes it, and
// drawing over an existing seam sews it back (spec scenario "Draw a seam").
class SeamSet {
public:
    void mark(EdgeId edge) { m_edges.insert(edge.value); }
    void erase(EdgeId edge) { m_edges.erase(edge.value); }
    // Sewing a seam back is the same operation as erasing it.
    void sew(EdgeId edge) { erase(edge); }
    // Drawing over an edge toggles its seam state (draw seams / sew back).
    void toggle(EdgeId edge) {
        if (isSeam(edge)) {
            erase(edge);
        } else {
            mark(edge);
        }
    }

    [[nodiscard]] bool isSeam(EdgeId edge) const {
        return m_edges.find(edge.value) != m_edges.end();
    }
    [[nodiscard]] std::size_t size() const { return m_edges.size(); }
    [[nodiscard]] bool empty() const { return m_edges.empty(); }
    void clear() { m_edges.clear(); }

private:
    std::unordered_set<Index> m_edges;
};

// Returns the shared edge between two consecutive island corners (invalid if
// the vertices are not connected).
[[nodiscard]] inline EdgeId faceEdge(const Mesh& mesh, VertexId a, VertexId b) {
    return mesh.edgeBetween(a, b);
}

// Face-connected components of the mesh where an edge may be crossed only if
// it is neither a seam nor a mesh boundary. A seam that fully rings a region
// therefore cuts it into its own island (spec: "fully cut islands"). Islands
// and the faces inside them are returned in deterministic (ascending id)
// order; every alive face appears in exactly one island.
[[nodiscard]] inline std::vector<std::vector<FaceId>> computeIslands(const Mesh& mesh,
                                                                     const SeamSet& seams) {
    std::vector<std::vector<FaceId>> islands;
    std::vector<bool> visited(mesh.faceCapacity(), false);

    for (Index start = 0; start < mesh.faceCapacity(); ++start) {
        const FaceId startFace{start};
        if (!mesh.isAlive(startFace) || visited[static_cast<std::size_t>(start)]) {
            continue;
        }
        std::vector<FaceId> island;
        std::vector<FaceId> stack{startFace};
        visited[static_cast<std::size_t>(start)] = true;

        while (!stack.empty()) {
            const FaceId face = stack.back();
            stack.pop_back();
            island.push_back(face);

            const std::vector<VertexId> verts = mesh.faceVertices(face);
            const std::size_t n = verts.size();
            for (std::size_t i = 0; i < n; ++i) {
                const EdgeId edge = mesh.edgeBetween(verts[i], verts[(i + 1) % n]);
                if (!edge.valid() || seams.isSeam(edge)) {
                    continue;
                }
                for (const FaceId neighbor : mesh.edgeFaces(edge)) {
                    if (neighbor == face || visited[static_cast<std::size_t>(neighbor.value)]) {
                        continue;
                    }
                    visited[static_cast<std::size_t>(neighbor.value)] = true;
                    stack.push_back(neighbor);
                }
            }
        }
        // Faces inside an island in ascending id order for determinism.
        std::sort(island.begin(), island.end(),
                  [](FaceId lhs, FaceId rhs) { return lhs.value < rhs.value; });
        islands.push_back(std::move(island));
    }
    return islands;
}

}  // namespace cyber::uv
