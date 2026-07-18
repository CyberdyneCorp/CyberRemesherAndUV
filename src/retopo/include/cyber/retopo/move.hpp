#pragma once

#include <queue>
#include <unordered_map>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/neighbors.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/relax.hpp"  // brushFalloff
#include "cyber/retopo/snapping.hpp"

// Move action (manual-retopology spec, "Core actions coherent across stages"):
// drag a region with surface-geodesic brush falloff. Distance is accumulated by
// Dijkstra over mesh edges from the seed, so the brush only reaches vertices
// through the surface and disconnected components are left untouched. Pinned
// vertices resist the move. Header-only and inline.
namespace cyber::retopo {

struct MoveParams {
    VertexId seed;             // grabbed vertex (brush centre on the surface)
    Vec3 displacement{};       // full drag applied at the seed, falling off outward
    float radius = 1.0f;       // geodesic reach of the brush
};

inline void move(Mesh& mesh, const MoveParams& params, const PinSet* pins = nullptr,
                 const SurfaceSnapper* snap = nullptr) {
    if (!mesh.isAlive(params.seed) || params.radius <= 0.0f) {
        return;
    }
    // Dijkstra over edge lengths from the seed (surface-geodesic approximation).
    std::unordered_map<Index, float> dist;
    using Entry = std::pair<float, Index>;  // (distance, vertex)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> frontier;
    dist[params.seed.value] = 0.0f;
    frontier.emplace(0.0f, params.seed.value);
    while (!frontier.empty()) {
        const auto [d, vi] = frontier.top();
        frontier.pop();
        const auto known = dist.find(vi);
        if (known != dist.end() && d > known->second) {
            continue;
        }
        const VertexId v{vi};
        const Vec3 vp = mesh.position(v);
        for (const VertexId nb : oneRing(mesh, v)) {
            const float nd = d + length(mesh.position(nb) - vp);
            if (nd > params.radius) {
                continue;
            }
            const auto it = dist.find(nb.value);
            if (it == dist.end() || nd < it->second) {
                dist[nb.value] = nd;
                frontier.emplace(nd, nb.value);
            }
        }
    }
    for (const auto& [vi, d] : dist) {
        const VertexId v{vi};
        if (pins != nullptr && pins->isPinned(v)) {
            continue;
        }
        const float w = brushFalloff(d, params.radius);
        const Vec3 target = mesh.position(v) + params.displacement * w;
        const Vec3 p =
            (snap != nullptr && !snap->empty()) ? snap->snapToSurface(target).point : target;
        mesh.setPosition(v, p);
    }
}

}  // namespace cyber::retopo
