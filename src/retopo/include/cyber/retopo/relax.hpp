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
                                      std::span<const VertexId> ring, std::span<const FaceId> faces,
                                      FaceNormalCache& normals) {
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
                               ExtraWeight extraWeight, const MeshAdjacency* adjacency = nullptr) {
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
                  const SurfaceSnapper* snap = nullptr, const MeshAdjacency* adjacency = nullptr) {
    detail::relaxSweep(mesh, params, pins, snap, 0.0f, [](VertexId) { return 1.0f; }, adjacency);
}

// Auto Relax, scoped to an edit (manual-retopology spec, "Auto Relax mode").
//
// The spec asks for "an automatic local relax of surrounding topology" after a
// topology-modifying operation — "the new and neighboring vertices". The
// whole-mesh autoRelax() in commands.hpp is NOT that: run after every stroke it
// would move topology the artist placed carefully on the far side of the model,
// and at 100k EditMesh vertices it would spend the interactive frame budget on
// vertices nobody touched.
//
// The region is TOPOLOGICAL, not spatial: every vertex within `rings` edge hops
// of a seed. A spatial sphere is the wrong shape for the edits this follows. A
// strip drawn down a thin limb is long and narrow, and a sphere large enough to
// cover it also reaches through to the other side of the limb — the same reason
// `move` is seeded from a vertex rather than a point in space.
//
// Weight falls off smoothly with ring distance, full at the seeds and zero one
// ring past `rings`, so the boundary between relaxed and untouched topology
// shows no step in quad size. Everything outside the region is left
// BIT-IDENTICAL: relaxSweep neither moves nor re-snaps a zero-weight vertex.
//
// Seeds that are dead are ignored. Returns the same distinct-vertex report as
// the weighted relax.
[[nodiscard]] inline ResnapReport relaxRegion(Mesh& mesh, std::span<const VertexId> seeds,
                                              int rings, const RelaxParams& params,
                                              const PinSet* pins = nullptr,
                                              const SurfaceSnapper* snap = nullptr,
                                              float resnapEpsilon = 0.0f) {
    constexpr int kUnreached = -1;
    std::vector<int> hops(mesh.vertexCapacity(), kUnreached);
    std::vector<VertexId> frontier;
    for (const VertexId seed : seeds) {
        if (mesh.isAlive(seed) && hops[seed.value] == kUnreached) {
            hops[seed.value] = 0;
            frontier.push_back(seed);
        }
    }
    if (frontier.empty() || rings < 0) {
        return {};
    }

    // Breadth-first over edge adjacency, one ring per pass.
    std::vector<VertexId> next;
    for (int ring = 1; ring <= rings && !frontier.empty(); ++ring) {
        next.clear();
        for (const VertexId v : frontier) {
            for (const VertexId n : oneRing(mesh, v)) {
                if (mesh.isAlive(n) && hops[n.value] == kUnreached) {
                    hops[n.value] = ring;
                    next.push_back(n);
                }
            }
        }
        frontier.swap(next);
    }

    RelaxParams scoped = params;
    scoped.brushRadius = 0.0f;  // the ring distance IS the mask; no spatial one on top
    const float span = static_cast<float>(rings + 1);
    return detail::relaxSweep(mesh, scoped, pins, snap, resnapEpsilon, [&](VertexId v) {
        const int h = v.value < hops.size() ? hops[v.value] : kUnreached;
        return h == kUnreached ? 0.0f : brushFalloff(static_cast<float>(h), span);
    });
}

}  // namespace cyber::retopo
