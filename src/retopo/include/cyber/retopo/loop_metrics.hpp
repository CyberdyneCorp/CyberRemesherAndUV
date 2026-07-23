#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/loops.hpp"
#include "cyber/retopo/snapping.hpp"

// Loop measurement for the Loop Info inspector (manual-retopology spec,
// "Loop Info inspection (vertex/edge counts, boundary length, snapping
// state in O(loop) time)"). One walk of the edge loop through the seed
// edge, then one pass over its edges — no global mesh scan, so the whole
// query is O(loop) as the spec requires.
//
// The metrics are deliberately descriptive, not corrective: nothing here
// mutates the mesh or the render cache.

namespace cyber::retopo {

// Everything the inspector chip shows about one edge loop. Distinct from
// build_tools.hpp's `LoopMetrics`/`loopInfo`, which measures a CALLER-
// SUPPLIED vertex list: this one owns the loop WALK and adds endpoints,
// boundary-edge count and Target snapping state.
struct EdgeLoopMetrics {
    // Edges in the loop, in walk order (the chain a loop tag colors).
    std::size_t edgeCount = 0;
    // Distinct vertices touched by those edges. A closed loop has exactly
    // as many vertices as edges; an open chain has one more.
    std::size_t vertexCount = 0;
    // True when the walk wrapped around (last edge meets the first).
    bool closed = false;
    // Summed edge length along the loop ("boundary length").
    float length = 0.0F;
    // Terminal vertices of an OPEN chain, in walk order. Empty for a
    // closed loop (which has no endpoints).
    std::optional<std::pair<VertexId, VertexId>> endpoints;
    // Loop edges that are mesh boundary edges (exactly one incident face).
    std::size_t boundaryEdgeCount = 0;
    // Snapping state against the Target, when a snapper is supplied:
    // how many loop vertices sit within `snapTolerance` of the Target
    // surface, and the largest deviation found. Without a snapper these
    // stay 0 / 0 and `snapMeasured` is false.
    bool snapMeasured = false;
    std::size_t snappedVertexCount = 0;
    float maxSnapDistance = 0.0F;
};

// Distance a vertex may sit off the Target and still count as snapped,
// expressed as a fraction of the loop's MEAN EDGE LENGTH so the verdict is
// scale-free (a 1cm gap is snapped on a building, adrift on a bolt).
inline constexpr float kLoopSnapToleranceFraction = 0.02F;

// Measures the edge loop through `seed`. `snapper` may be null (no Target
// loaded): then the snapping fields stay unmeasured. A dead seed edge
// yields a zeroed result (edgeCount 0), which callers read as "no loop".
[[nodiscard]] inline EdgeLoopMetrics measureEdgeLoop(const Mesh& mesh, EdgeId seed,
                                                     const SurfaceSnapper* snapper) {
    EdgeLoopMetrics metrics;
    const std::vector<EdgeId> loop = edgeLoopFrom(mesh, seed);
    if (loop.empty()) {
        return metrics;
    }
    metrics.edgeCount = loop.size();

    // Ordered distinct vertices along the walk. edgeLoopFrom returns the
    // chain in walk order, so consecutive edges share a vertex; collecting
    // both endpoints of every edge in order and de-duplicating gives the
    // vertex chain in O(loop).
    std::vector<VertexId> ordered;
    ordered.reserve(loop.size() + 1);
    std::unordered_set<Index> seen;
    const auto push = [&](VertexId v) {
        if (seen.insert(v.value).second) {
            ordered.push_back(v);
        }
    };
    for (const EdgeId e : loop) {
        const std::pair<VertexId, VertexId> ends = mesh.edgeVertices(e);
        push(ends.first);
        push(ends.second);
        metrics.length += length(mesh.position(ends.second) - mesh.position(ends.first));
        if (mesh.isBoundaryEdge(e)) {
            ++metrics.boundaryEdgeCount;
        }
    }
    metrics.vertexCount = ordered.size();
    // A closed loop revisits its first vertex, so it has exactly as many
    // distinct vertices as edges; an open chain has one more.
    metrics.closed = loop.size() > 2 && ordered.size() == loop.size();
    if (!metrics.closed && ordered.size() >= 2) {
        metrics.endpoints = std::make_pair(ordered.front(), ordered.back());
    }

    if (snapper != nullptr && !ordered.empty()) {
        metrics.snapMeasured = true;
        const float meanEdge = metrics.length / static_cast<float>(loop.size());
        const float tolerance = meanEdge * kLoopSnapToleranceFraction;
        for (const VertexId v : ordered) {
            const SurfaceHit hit = snapper->snapToSurface(mesh.position(v));
            const float distance = std::sqrt(hit.distanceSquared);
            if (distance > metrics.maxSnapDistance) {
                metrics.maxSnapDistance = distance;
            }
            if (distance <= tolerance) {
                ++metrics.snappedVertexCount;
            }
        }
    }
    return metrics;
}

}  // namespace cyber::retopo
