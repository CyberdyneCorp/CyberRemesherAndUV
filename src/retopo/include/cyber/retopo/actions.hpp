#pragma once

#include <optional>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/snapping.hpp"

// Pencil stroke-grammar operations (manual-retopology spec, "Pencil stroke
// grammar" + "Advanced build tools"), expressed as concrete edits over
// cyber::Mesh and reusing its structural operators (addFace, splitEdge,
// splitFace, flipEdge, collapseEdge, removeFace). New vertices snap to the
// Target surface when a SurfaceSnapper is supplied. These are the deterministic
// geometric primitives underneath the (best-effort) stroke recognizer.
namespace cyber::retopo {

// Adds a vertex, snapping to the Target surface first when `snap` is non-null.
[[nodiscard]] VertexId addSnappedVertex(Mesh& mesh, Vec3 position, const SurfaceSnapper* snap);

// Closed quad/tri shape -> create face. `points` must hold 3 or 4 non-repeating
// corner positions in order; returns an invalid id on degenerate input.
[[nodiscard]] FaceId createFace(Mesh& mesh, std::span<const Vec3> points,
                                const SurfaceSnapper* snap = nullptr);

// BuildQuad: extrude a single quad from a boundary edge to two new corner
// positions (`farA` opposite the edge's first vertex, `farB` opposite the
// second). Returns the new face, or an invalid id if the edge is unusable.
[[nodiscard]] FaceId extrudeEdge(Mesh& mesh, EdgeId boundary, Vec3 farA, Vec3 farB,
                                 const SurfaceSnapper* snap = nullptr);

// ExtendBoundary: extrude a quad strip from a run of boundary edges by a shared
// `offset`, welding new vertices shared between adjacent edges. Returns the new
// faces (may be empty).
[[nodiscard]] std::vector<FaceId> extrudeBoundary(Mesh& mesh, std::span<const EdgeId> boundary,
                                                  Vec3 offset, const SurfaceSnapper* snap = nullptr);

// Bridge two equal-length boundary vertex sequences with a band of quads.
// Returns the new faces; empty if the sequences differ in length or are too
// short.
[[nodiscard]] std::vector<FaceId> bridgeLoops(Mesh& mesh, std::span<const VertexId> loopA,
                                              std::span<const VertexId> loopB);

// Insert an edge loop through a quad: splits `across` and the quad's opposite
// edge at their midpoints and connects the two new vertices, yielding two
// quads. Returns the newly created face, or an invalid id on failure.
[[nodiscard]] FaceId insertLoop(Mesh& mesh, FaceId quad, EdgeId across);

// Scribble/X delete: removes every listed face.
void deleteFaces(Mesh& mesh, std::span<const FaceId> faces);

// Straight line between two vertices -> merge/collapse the shared edge (two
// adjacent triangles become one quad). Returns false if the edge is unusable.
bool mergePair(Mesh& mesh, EdgeId edge);

// Circle over an edge -> rotate it (redirect loop flow); flips the shared edge
// of two triangles. Returns false if the edge is not flippable.
bool rotateEdge(Mesh& mesh, EdgeId edge);

// Closed loop over a cylindrical region -> extrude a cylinder: builds a band of
// quads connecting `ring` to a copy offset by `direction` (both rings snapped).
// `ring` is an ordered, closed polyline. Returns the band faces.
[[nodiscard]] std::vector<FaceId> extrudeCylinder(Mesh& mesh, std::span<const Vec3> ring,
                                                  Vec3 direction, const SurfaceSnapper* snap = nullptr);

}  // namespace cyber::retopo
