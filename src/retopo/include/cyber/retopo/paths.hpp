#pragma once

#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Edge-walk paths over the EditMesh (manual-retopology spec, "Advanced build
// tools": Path Distribute straightens/evenly distributes vertices along the
// CLOSEST PATH between the stroke's first and last vertex). Header-only and
// deterministic: Dijkstra over live edges with Euclidean weights; ties break
// toward the lower vertex id via the priority queue's (distance, id) order.
namespace cyber::retopo {

// Shortest edge path between two live vertices, endpoints inclusive and in
// from->to order. Empty when either vertex is dead, from == to, or no path
// exists (disconnected components).
[[nodiscard]] inline std::vector<VertexId> shortestVertexPath(const Mesh& mesh, VertexId from,
                                                              VertexId to) {
    std::vector<VertexId> path;
    if (!mesh.isAlive(from) || !mesh.isAlive(to) || from == to) {
        return path;
    }
    using Entry = std::pair<float, Index>;  // (distance, vertex id)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
    std::unordered_map<Index, float> distance;
    std::unordered_map<Index, Index> previous;
    distance[from.value] = 0.0f;
    frontier.emplace(0.0f, from.value);
    while (!frontier.empty()) {
        const auto [d, u] = frontier.top();
        frontier.pop();
        const auto known = distance.find(u);
        if (known != distance.end() && d > known->second) {
            continue;  // stale queue entry
        }
        if (u == to.value) {
            break;
        }
        for (const EdgeId e : mesh.vertexEdges(VertexId{u})) {
            if (!mesh.isAlive(e)) {
                continue;
            }
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId v = a.value == u ? b : a;
            const float step = length(mesh.position(v) - mesh.position(VertexId{u}));
            const float candidate = d + step;
            const auto it = distance.find(v.value);
            if (it == distance.end() || candidate < it->second) {
                distance[v.value] = candidate;
                previous[v.value] = u;
                frontier.emplace(candidate, v.value);
            }
        }
    }
    if (distance.find(to.value) == distance.end()) {
        return path;
    }
    for (Index v = to.value;;) {
        path.push_back(VertexId{v});
        if (v == from.value) {
            break;
        }
        const auto it = previous.find(v);
        if (it == previous.end()) {
            return {};  // unreachable (defensive: distance said otherwise)
        }
        v = it->second;
    }
    std::vector<VertexId> ordered(path.rbegin(), path.rend());
    return ordered;
}

}  // namespace cyber::retopo
