#pragma once

#include <optional>
#include <utility>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// EditMesh element picking (manual-retopology spec, "Pencil stroke grammar"
// context resolution): nearest-vertex and nearest-edge queries against the
// *EditMesh*. Deliberately brute force — the EditMesh is the low-poly cage
// (hundreds to a few thousand elements), a linear scan is faster than
// (re)building an acceleration structure after every mutation, and results
// are deterministic (ties break toward the lower element id). The Target
// keeps using the BVH-backed SurfaceSnapper.
namespace cyber::retopo {

struct VertexPick {
    VertexId vertex;
    Vec3 position;
    float distanceSquared = 0.0f;
};

struct EdgePick {
    EdgeId edge;
    Vec3 point;  // closest point on the edge segment
    float distanceSquared = 0.0f;
};

// Closest point on segment [a, b] to `p`.
[[nodiscard]] inline Vec3 closestPointOnSegment(Vec3 a, Vec3 b, Vec3 p) {
    const Vec3 ab = b - a;
    const float len2 = lengthSquared(ab);
    if (len2 <= 0.0f) {
        return a;
    }
    const float t = std::fmax(0.0f, std::fmin(1.0f, dot(p - a, ab) / len2));
    return a + ab * t;
}

// Nearest live vertex within `maxDistance` of `query`, skipping `exclude`
// when given; nullopt if none. The exclusion exists for merge-snap
// detection while a vertex is being dragged (pencil-interaction spec,
// "Snap feedback"): the dragged vertex sits exactly at the query point, so
// an unfiltered nearest query could only ever return it.
[[nodiscard]] inline std::optional<VertexPick> nearestVertex(
    const Mesh& mesh, Vec3 query, float maxDistance,
    std::optional<VertexId> exclude = std::nullopt) {
    std::optional<VertexPick> best;
    const float limit = maxDistance * maxDistance;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v) || (exclude && *exclude == v)) {
            continue;
        }
        const Vec3 p = mesh.position(v);
        const float d2 = lengthSquared(p - query);
        if (d2 <= limit && (!best || d2 < best->distanceSquared)) {
            best = VertexPick{v, p, d2};
        }
    }
    return best;
}

// Nearest live edge (closest point on its segment) within `maxDistance` of
// `query`; nullopt if none.
[[nodiscard]] inline std::optional<EdgePick> nearestEdge(const Mesh& mesh, Vec3 query,
                                                         float maxDistance) {
    std::optional<EdgePick> best;
    const float limit = maxDistance * maxDistance;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [v0, v1] = mesh.edgeVertices(e);
        const Vec3 point =
            closestPointOnSegment(mesh.position(v0), mesh.position(v1), query);
        const float d2 = lengthSquared(point - query);
        if (d2 <= limit && (!best || d2 < best->distanceSquared)) {
            best = EdgePick{e, point, d2};
        }
    }
    return best;
}

}  // namespace cyber::retopo
