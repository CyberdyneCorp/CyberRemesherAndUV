#pragma once

#include <unordered_set>
#include <vector>

#include "cyber/core/mesh.hpp"

// Boundary-loop walks (manual-retopology spec: Extend Boundary's boundary
// auto-select and the Loop Info inspector's boundary metrics). The existing
// loop walks (loops.hpp) deliberately STOP at boundaries — quad-ring and
// edge-loop topology is interior-only — so boundary chains need their own
// deterministic walk over edges with exactly one incident face.
// Header-only and inline, read-only over the mesh.
namespace cyber::retopo {

// An ordered boundary chain: vertices along the boundary, and whether the
// walk closed back onto its first vertex (a full boundary loop of a hole /
// an open shell's rim).
struct BoundaryChain {
    std::vector<VertexId> vertices;
    bool closed = false;
};

// True for a live edge with exactly one incident face.
[[nodiscard]] inline bool isBoundaryEdge(const Mesh& mesh, EdgeId e) {
    return mesh.isAlive(e) && mesh.edgeFaces(e).size() == 1;
}

namespace detail {

// The unique OTHER boundary edge incident to `v`, or an invalid id when the
// boundary does not continue simply through `v` (an end of an open chain, or
// a non-manifold pinch where 3+ boundary edges meet — the walk stops there
// deterministically instead of picking a branch).
inline EdgeId nextBoundaryEdge(const Mesh& mesh, VertexId v, EdgeId current) {
    EdgeId next{};
    int count = 0;
    for (const EdgeId e : mesh.vertexEdges(v)) {
        if (e.value == current.value || !isBoundaryEdge(mesh, e)) {
            continue;
        }
        ++count;
        next = e;
    }
    return count == 1 ? next : EdgeId{};
}

// The endpoint of `e` that is not `v`.
inline VertexId otherVertex(const Mesh& mesh, EdgeId e, VertexId v) {
    const auto [a, b] = mesh.edgeVertices(e);
    return a.value == v.value ? b : a;
}

}  // namespace detail

// Walks the boundary chain through `seed` (which must be a boundary edge):
// follows boundary edges in both directions until the walk closes onto
// itself (closed loop) or stops at chain ends / non-manifold pinches.
// Deterministic: the chain starts from the seed's first endpoint and runs
// toward its second; open chains are then extended backwards from the first
// endpoint (prepended in walk order). Returns an empty chain when `seed` is
// dead or not a boundary edge.
[[nodiscard]] inline BoundaryChain boundaryChain(const Mesh& mesh, EdgeId seed) {
    BoundaryChain chain;
    if (!isBoundaryEdge(mesh, seed)) {
        return chain;
    }
    const auto [first, second] = mesh.edgeVertices(seed);
    std::unordered_set<Index> visited;  // edges consumed by the walk
    visited.insert(seed.value);
    chain.vertices.push_back(first);
    chain.vertices.push_back(second);

    // Forward: from `second` away from the seed.
    VertexId cursor = second;
    EdgeId arrived = seed;
    // Bounded by the edge capacity: every step consumes a distinct edge.
    for (Index step = 0; step < mesh.edgeCapacity(); ++step) {
        const EdgeId next = detail::nextBoundaryEdge(mesh, cursor, arrived);
        if (!next.valid() || visited.count(next.value) != 0) {
            break;
        }
        visited.insert(next.value);
        cursor = detail::otherVertex(mesh, next, cursor);
        if (cursor.value == first.value) {
            chain.closed = true;
            return chain;  // walk closed: every boundary vertex collected
        }
        chain.vertices.push_back(cursor);
        arrived = next;
    }

    // Open chain: extend backwards from `first`, prepending.
    cursor = first;
    arrived = seed;
    std::vector<VertexId> prefix;
    for (Index step = 0; step < mesh.edgeCapacity(); ++step) {
        const EdgeId next = detail::nextBoundaryEdge(mesh, cursor, arrived);
        if (!next.valid() || visited.count(next.value) != 0) {
            break;
        }
        visited.insert(next.value);
        cursor = detail::otherVertex(mesh, next, cursor);
        prefix.push_back(cursor);
        arrived = next;
    }
    chain.vertices.insert(chain.vertices.begin(), prefix.rbegin(), prefix.rend());
    return chain;
}

}  // namespace cyber::retopo
