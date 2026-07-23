#pragma once

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "cyber/core/mesh.hpp"

// Quad-loop topology walks (manual-retopology gesture grammar, app task 3.4):
//
//   * quadRingFromEdge — the RING of quads a "line across a face ring"
//     stroke targets: starting from one crossed edge, walk across each quad
//     to its opposite edge and continue until the ring closes or hits a
//     boundary / non-quad face in both directions.
//   * edgeLoopFrom — the EDGE LOOP a "line along a loop" stroke tags: from
//     one edge, continue through each regular (valence-4 interior) vertex
//     along the topologically opposite edge until the loop closes or ends.
//   * insertLoopAcrossRing — the full-ring version of insertLoop
//     (actions.hpp splits exactly one quad): splits every ring edge at `t`
//     and every ring quad between consecutive midpoints, inserting a
//     complete edge loop around the ring in one operation.
//
// All walks are deterministic: neighbors are visited in the mesh's stored
// (insertion) order, and results are reported in walk order starting from
// the seed.
namespace cyber::retopo {

// Ring of quads crossed by consecutive "across" edges. For a closed ring
// edges.size() == faces.size(); for an open ring (boundary/non-quad stops
// the walk) edges.size() == faces.size() + 1. edges[i] and edges[i+1]
// (cyclically, when closed) are opposite edges of faces[i].
struct QuadRing {
    std::vector<EdgeId> edges;
    std::vector<FaceId> faces;
    bool closed = false;
};

namespace loop_detail {

// Edge opposite `across` in a quad (via the vertex cycle). Invalid when the
// face is not a quad or does not contain `across`.
inline EdgeId oppositeQuadEdge(const Mesh& mesh, FaceId quad, EdgeId across) {
    const std::vector<VertexId> vs = mesh.faceVertices(quad);
    if (vs.size() != 4) {
        return EdgeId{};
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (mesh.edgeBetween(vs[i], vs[(i + 1) % 4]) == across) {
            return mesh.edgeBetween(vs[(i + 2) % 4], vs[(i + 3) % 4]);
        }
    }
    return EdgeId{};
}

}  // namespace loop_detail

// Walks the quad ring through `start`. Returns an empty ring when `start`
// is dead. A lone quad yields faces = {quad}, edges = {start, opposite}.
[[nodiscard]] inline QuadRing quadRingFromEdge(const Mesh& mesh, EdgeId start) {
    QuadRing ring;
    if (!mesh.isAlive(start)) {
        return ring;
    }
    ring.edges.push_back(start);
    std::unordered_set<Index> visited;

    // Walk one direction: repeatedly cross the first unvisited quad of the
    // frontier edge. `forward` appends, backward prepends (so the ring
    // stays contiguous in walk order).
    const auto walk = [&](bool forward) {
        EdgeId frontier = forward ? ring.edges.back() : ring.edges.front();
        while (true) {
            FaceId next{};
            for (const FaceId f : mesh.edgeFaces(frontier)) {
                if (mesh.faceSize(f) == 4 && visited.find(f.value) == visited.end()) {
                    next = f;
                    break;
                }
            }
            if (!next.valid()) {
                return;
            }
            const EdgeId opposite = loop_detail::oppositeQuadEdge(mesh, next, frontier);
            if (!opposite.valid()) {
                return;
            }
            visited.insert(next.value);
            if (forward) {
                ring.faces.push_back(next);
            } else {
                ring.faces.insert(ring.faces.begin(), next);
            }
            if (opposite == start) {
                ring.closed = true;
                return;
            }
            if (forward) {
                ring.edges.push_back(opposite);
            } else {
                ring.edges.insert(ring.edges.begin(), opposite);
            }
            frontier = opposite;
        }
    };
    walk(true);
    if (!ring.closed) {
        walk(false);
    }
    return ring;
}

// Walks the edge loop through `start` (the chain a loop tag colors). At
// each endpoint the loop continues along the edge that shares NO face with
// the current edge — the classic quad-mesh loop rule — and stops at
// boundaries, poles (valence != 4), or when the loop closes.
[[nodiscard]] inline std::vector<EdgeId> edgeLoopFrom(const Mesh& mesh, EdgeId start) {
    std::vector<EdgeId> loop;
    if (!mesh.isAlive(start)) {
        return loop;
    }
    loop.push_back(start);
    std::unordered_set<Index> visited{start.value};

    const auto nextAlong = [&](EdgeId e, VertexId v) -> EdgeId {
        if (mesh.vertexEdges(v).size() != 4 || mesh.edgeFaceCount(e) != 2) {
            return EdgeId{};  // pole or boundary: the loop ends here
        }
        const std::vector<FaceId> faces = mesh.edgeFaces(e);
        for (const EdgeId candidate : mesh.vertexEdges(v)) {
            if (candidate == e) {
                continue;
            }
            bool sharesFace = false;
            for (const FaceId f : mesh.edgeFaces(candidate)) {
                if (f == faces[0] || f == faces[1]) {
                    sharesFace = true;
                    break;
                }
            }
            if (!sharesFace) {
                return candidate;
            }
        }
        return EdgeId{};
    };

    const auto walk = [&](bool forward) {
        EdgeId e = forward ? loop.back() : loop.front();
        VertexId v = forward ? mesh.edgeVertices(e).second : mesh.edgeVertices(e).first;
        while (true) {
            const EdgeId next = nextAlong(e, v);
            if (!next.valid() || visited.find(next.value) != visited.end()) {
                return;
            }
            visited.insert(next.value);
            if (forward) {
                loop.push_back(next);
            } else {
                loop.insert(loop.begin(), next);
            }
            const auto [n0, n1] = mesh.edgeVertices(next);
            v = n0 == v ? n1 : n0;
            e = next;
        }
    };
    walk(true);
    walk(false);
    return loop;
}

// Result of a full-ring loop insert.
struct LoopInsertResult {
    std::vector<VertexId> newVertices;  // one midpoint per split ring edge
    std::vector<FaceId> newFaces;       // one new quad per split ring face
};

// Inserts a complete edge loop around the quad ring through `start`: every
// ring edge is split at `t` (0..1 along the edge's stored orientation) and
// every ring quad is split between its two midpoints. Returns an empty
// result (mesh untouched) when `start` is dead or borders no quad.
[[nodiscard]] inline LoopInsertResult insertLoopAcrossRing(Mesh& mesh, EdgeId start,
                                                           float t = 0.5f) {
    LoopInsertResult result;
    const QuadRing ring = quadRingFromEdge(mesh, start);
    if (ring.faces.empty()) {
        return result;
    }
    result.newVertices.reserve(ring.edges.size());
    for (const EdgeId e : ring.edges) {
        result.newVertices.push_back(mesh.splitEdge(e, t));
    }
    result.newFaces.reserve(ring.faces.size());
    for (std::size_t i = 0; i < ring.faces.size(); ++i) {
        const VertexId entry = result.newVertices[i];
        const VertexId exit = result.newVertices[(i + 1) % result.newVertices.size()];
        const FaceId half = mesh.splitFace(ring.faces[i], entry, exit);
        if (half.valid()) {
            result.newFaces.push_back(half);
        }
    }
    return result;
}

}  // namespace cyber::retopo
