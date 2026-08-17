#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/adjacency.hpp"
#include "cyber/retopo/neighbors.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/snapping.hpp"

// Relax action (manual-retopology spec, "Core actions coherent across stages"):
// tangential Laplacian smoothing that keeps vertices on the Target surface.
// Grid corners are auto-pinned so regular patterns survive, a brush radius
// masks the affected region (visible-radius mask), and explicit pins are
// honoured. Header-only and inline.
namespace cyber::retopo {

struct RelaxParams {
    float strength = 0.5f;       // per-iteration blend toward the tangent target
    int iterations = 1;          // Jacobi sweeps
    Vec3 brushCenter{};          // visible-radius mask centre
    float brushRadius = 0.0f;    // <= 0 relaxes the whole mesh (no mask)
    bool autoPinCorners = true;  // pin low-valence (grid-corner) vertices
};

// Smooth 0..1 brush falloff; 1 at the centre, 0 at the radius.
[[nodiscard]] inline float brushFalloff(float distance, float radius) {
    if (radius <= 0.0f) {
        return 1.0f;
    }
    if (distance >= radius) {
        return 0.0f;
    }
    const float t = 1.0f - distance / radius;
    return t * t * (3.0f - 2.0f * t);  // smoothstep
}

// A vertex counts as a grid corner (auto-pin candidate) when its valence is at
// most two, which preserves the corners of a regular quad grid under Relax.
[[nodiscard]] inline bool isGridCorner(const Mesh& mesh, VertexId v) {
    return oneRing(mesh, v).size() <= 2;
}

namespace detail {

// A vertex is exempt from a relax sweep when it is dead, pinned, an auto-pinned
// grid corner, or outside the brush mask. `ringSize` is the vertex's one-ring
// size, which the caller has already gathered for the sweep itself.
[[nodiscard]] inline bool relaxSkips(const Mesh& mesh, VertexId v, std::size_t ringSize,
                                     const RelaxParams& params, const PinSet* pins) {
    if (!mesh.isAlive(v)) {
        return true;
    }
    if (pins != nullptr && pins->isPinned(v)) {
        return true;
    }
    if (params.autoPinCorners && ringSize <= 2) {
        return true;
    }
    const float radiusSquared = params.brushRadius * params.brushRadius;
    return params.brushRadius > 0.0f &&
           lengthSquared(mesh.position(v) - params.brushCenter) > radiusSquared;
}

// Tangential Laplacian target for `v`, blended by `w` toward the one-ring
// centroid with the normal component removed.
[[nodiscard]] inline Vec3 relaxTarget(const Mesh& mesh, VertexId v, float w,
                                      std::span<const VertexId> ring,
                                      std::span<const FaceId> faces, FaceNormalCache& normals) {
    const Vec3 pos = mesh.position(v);
    Vec3 delta = ringCentroid(mesh, ring, pos) - pos;
    const Vec3 n = ringNormal(normals, faces);
    delta = delta - n * dot(delta, n);  // keep the move tangential
    return pos + delta * w;
}

// Tallies `v` at most once for the whole sweep. The iterations revisit the same
// vertices, and ResnapReport counts DISTINCT vertices, not writes.
inline void countVertexOnce(std::vector<std::uint8_t>& seen, VertexId v, std::size_t& counter) {
    if (v.value < seen.size() && seen[v.value] == 0) {
        seen[v.value] = 1;
        ++counter;
    }
}

// The relax sweep shared by plain `relax()` and the weighted relax in
// soft_selection.hpp. `extraWeight(v)` scales the per-vertex blend on top of the
// brush falloff; a value <= 0 leaves the vertex COMPLETELY untouched — it is
// neither moved nor re-snapped, which is what makes zero-weight vertices
// bit-identical under a weighted relax. Passing a constant 1 reproduces the
// unweighted sweep exactly (multiplying by 1.0f is bit-neutral).
//
// `adjacency`, when given, must describe `mesh`'s current topology: the sweep
// reads its one-ring and vertex->face lists instead of gathering them per
// vertex, which is what keeps a drag frame off the allocator. Passing nullptr
// gathers them from the mesh and computes exactly the same result.
template <typename ExtraWeight>
inline ResnapReport relaxSweep(Mesh& mesh, const RelaxParams& params, const PinSet* pins,
                               const SurfaceSnapper* snap, float resnapEpsilon,
                               ExtraWeight extraWeight,
                               const MeshAdjacency* adjacency = nullptr) {
    ResnapReport report;
    const bool snapping = snap != nullptr && !snap->empty();
    RingSource rings(mesh, adjacency);
    FaceNormalCache faceNormals(mesh);
    std::vector<std::uint8_t> movedSeen(mesh.vertexCapacity(), 0);
    std::vector<std::uint8_t> resnappedSeen(mesh.vertexCapacity(), 0);
    std::vector<std::pair<VertexId, Vec3>> updates;
    for (int iter = 0; iter < params.iterations; ++iter) {
        updates.clear();
        faceNormals.reset();
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v)) {
                continue;
            }
            const std::span<const VertexId> ring = rings.ring(v);
            if (relaxSkips(mesh, v, ring.size(), params, pins)) {
                continue;
            }
            const float extra = extraWeight(v);
            if (extra <= 0.0f) {
                continue;
            }
            const Vec3 pos = mesh.position(v);
            const float w = params.strength * extra *
                            brushFalloff(length(pos - params.brushCenter), params.brushRadius);
            updates.emplace_back(v, relaxTarget(mesh, v, w, ring, rings.faces(v), faceNormals));
        }
        for (const auto& [v, target] : updates) {
            countVertexOnce(movedSeen, v, report.moved);
            if (!snapping) {
                mesh.setPosition(v, target);
                continue;
            }
            const Vec3 p = snap->snapToSurface(target).point;
            const float pulled = length(p - target);
            if (pulled > resnapEpsilon) {
                countVertexOnce(resnappedSeen, v, report.resnapped);
                report.maxSnapDistance = std::max(report.maxSnapDistance, pulled);
            }
            mesh.setPosition(v, p);
        }
    }
    return report;
}

}  // namespace detail

inline void relax(Mesh& mesh, const RelaxParams& params, const PinSet* pins = nullptr,
                  const SurfaceSnapper* snap = nullptr,
                  const MeshAdjacency* adjacency = nullptr) {
    detail::relaxSweep(
        mesh, params, pins, snap, 0.0f, [](VertexId) { return 1.0f; }, adjacency);
}

}  // namespace cyber::retopo
