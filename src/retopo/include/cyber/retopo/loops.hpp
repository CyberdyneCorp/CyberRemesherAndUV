#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/snapping.hpp"

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

// Result of a loop slide.
struct LoopSlideResult {
    std::size_t loopVertices = 0;  // vertices on the loop
    std::size_t moved = 0;         // of those, how many had a neighbour to slide toward
};

namespace detail {

// The face whose winding traverses the DIRECTED edge a->b, if any.
//
// build_tools' faceTraverses answers a narrower question — it only looks at
// boundary edges and returns false as soon as an edge has two faces — which is
// right for welding onto a boundary and wrong here: a loop slide runs through
// interior quads, where every loop edge has a face on each side.
[[nodiscard]] inline std::optional<std::vector<VertexId>> faceAlong(const Mesh& mesh, VertexId a,
                                                                    VertexId b) {
    const EdgeId e = mesh.edgeBetween(a, b);
    if (!e.valid()) {
        return std::nullopt;
    }
    for (const FaceId f : mesh.edgeFaces(e)) {
        std::vector<VertexId> ring = mesh.faceVertices(f);
        for (std::size_t i = 0; i < ring.size(); ++i) {
            if (ring[i].value == a.value && ring[(i + 1) % ring.size()].value == b.value) {
                return ring;
            }
        }
    }
    return std::nullopt;
}

// The vertices of an edge loop, in walk order. A closed loop repeats no vertex;
// an open one has one more vertex than edges.
[[nodiscard]] inline std::pair<std::vector<VertexId>, bool> loopVertices(
    const Mesh& mesh, const std::vector<EdgeId>& loop) {
    std::vector<VertexId> chain;
    if (loop.empty()) {
        return {chain, false};
    }
    auto [a0, b0] = mesh.edgeVertices(loop.front());
    if (loop.size() == 1) {
        return {{a0, b0}, false};
    }
    // Start at the end of edge 0 that does NOT touch edge 1, so the chain walks
    // into the loop rather than out of it.
    const auto [a1, b1] = mesh.edgeVertices(loop[1]);
    const bool b0Shared = b0.value == a1.value || b0.value == b1.value;
    VertexId current = b0Shared ? a0 : b0;
    chain.push_back(current);
    for (const EdgeId e : loop) {
        const auto [ea, eb] = mesh.edgeVertices(e);
        current = ea.value == current.value ? eb : ea;
        chain.push_back(current);
    }
    const bool closed = chain.front().value == chain.back().value;
    if (closed) {
        chain.pop_back();
    }
    return {chain, closed};
}

}  // namespace detail

// Loop slide (manual-retopology spec, "Pencil stroke grammar": double-tap an edge
// loop to slide it). Moves every vertex of the edge loop through `edge` a
// fraction `t` of the way along its rail toward the neighbouring loop.
//
// tweak.hpp's slideVertex moves ONE vertex toward a neighbour the caller names.
// A loop slide cannot be built by calling it per vertex, because the hard part
// is not the move: it is choosing, for every vertex, a neighbour on the SAME
// SIDE of the loop. Pick independently and a slide on a closed ring twists half
// the loop one way and half the other.
//
// Sides come from orientation. Walking the loop a->b, the face whose winding
// traverses a->b always lies on the same side of the loop in a consistently
// oriented mesh; in that quad [a, b, c, d] the rails are b-c and a-d. So `t > 0`
// slides toward that side and `t < 0` toward the face traversing b->a.
//
// Targets are computed from the ORIGINAL positions before any vertex moves, so
// the result does not depend on the order the loop is walked.
//
// |t| must be below 1: at 1 the loop lands on its neighbour and every rail edge
// collapses to zero length. A vertex with no quad on the requested side — the
// outer edge of a boundary loop — stays put and is not counted, so a boundary
// loop slides inward and not outward. Moved vertices snap to the Target when
// `snap` is given. The mesh is unchanged when `edge` is dead or |t| >= 1.
[[nodiscard]] inline LoopSlideResult slideLoop(Mesh& mesh, EdgeId edge, float t,
                                               const SurfaceSnapper* snap = nullptr) {
    LoopSlideResult result;
    if (!mesh.isAlive(edge) || !(std::abs(t) < 1.0f)) {
        return result;
    }
    const std::vector<EdgeId> loop = edgeLoopFrom(mesh, edge);
    const auto [chain, closed] = detail::loopVertices(mesh, loop);
    result.loopVertices = chain.size();
    if (chain.size() < 2 || t == 0.0f) {
        return result;
    }

    const bool positive = t > 0.0f;
    const float amount = std::abs(t);
    std::vector<std::optional<VertexId>> toward(chain.size());

    // For each directed loop edge, read the rail neighbours off the face on the
    // requested side. A regular vertex is visited by both of its loop edges and
    // both agree; the first answer is kept, which keeps the result
    // deterministic at an irregular vertex where they would not.
    const std::size_t edges = closed ? chain.size() : chain.size() - 1;
    for (std::size_t i = 0; i < edges; ++i) {
        const std::size_t j = (i + 1) % chain.size();
        const VertexId a = positive ? chain[i] : chain[j];
        const VertexId b = positive ? chain[j] : chain[i];
        const auto face = detail::faceAlong(mesh, a, b);
        if (!face || face->size() != 4) {
            continue;  // no quad on this side: a boundary, or not a quad ring
        }
        // Rotate so the ring reads [a, b, c, d]; the rails are then b-c and a-d.
        std::vector<VertexId> q = *face;
        while (q[0].value != a.value) {
            std::rotate(q.begin(), q.begin() + 1, q.end());
        }
        const std::size_t ia = positive ? i : j;
        const std::size_t ib = positive ? j : i;
        if (!toward[ia]) {
            toward[ia] = q[3];
        }
        if (!toward[ib]) {
            toward[ib] = q[2];
        }
    }

    std::vector<std::pair<VertexId, Vec3>> targets;
    targets.reserve(chain.size());
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (toward[i] && mesh.isAlive(*toward[i])) {
            targets.emplace_back(chain[i],
                                 lerp(mesh.position(chain[i]), mesh.position(*toward[i]), amount));
        }
    }
    const bool snapping = snap != nullptr && !snap->empty();
    for (const auto& [v, target] : targets) {
        mesh.setPosition(v, snapping ? snap->snapToSurface(target).point : target);
    }
    result.moved = targets.size();
    return result;
}

}  // namespace cyber::retopo
