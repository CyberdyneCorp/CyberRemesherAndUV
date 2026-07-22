#pragma once

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/snapping.hpp"

// Tweak action (manual-retopology spec): manipulate single elements. Moving a
// vertex reprojects it onto the Target surface on release ("Moved vertex
// reprojects"); the vertex-snap modifier snaps to the nearest Target vertex
// instead ("Vertex-snap modifier"). Loop slide nudges a vertex along one of its
// edges. Tweak ignores pins by design (pinned vertices stay movable by explicit
// Tweak). Header-only and inline.
namespace cyber::retopo {

// Drops `v` at `target`, reprojected to the Target surface.
inline void tweakVertex(Mesh& mesh, VertexId v, Vec3 target, const SurfaceSnapper* snap = nullptr) {
    if (!mesh.isAlive(v)) {
        return;
    }
    const Vec3 p = (snap != nullptr && !snap->empty()) ? snap->snapToSurface(target).point : target;
    mesh.setPosition(v, p);
}

// Drops `v` at `target`, snapped to the nearest Target vertex within `radius`;
// falls back to surface snapping when no Target vertex is close enough. Returns
// true when a Target vertex was used.
inline bool tweakVertexToTargetVertex(Mesh& mesh, VertexId v, Vec3 target,
                                      const SurfaceSnapper& snap, float radius) {
    if (!mesh.isAlive(v)) {
        return false;
    }
    if (const auto hit = snap.snapToVertex(target, radius)) {
        mesh.setPosition(v, hit->point);
        return true;
    }
    mesh.setPosition(v, snap.snapToSurface(target).point);
    return false;
}

// Slides `v` a fraction `t` of the way toward neighbour `toward` (loop-slide
// primitive), reprojecting onto the surface.
inline void slideVertex(Mesh& mesh, VertexId v, VertexId toward, float t,
                        const SurfaceSnapper* snap = nullptr) {
    if (!mesh.isAlive(v) || !mesh.isAlive(toward)) {
        return;
    }
    const Vec3 target = lerp(mesh.position(v), mesh.position(toward), t);
    tweakVertex(mesh, v, target, snap);
}

}  // namespace cyber::retopo
