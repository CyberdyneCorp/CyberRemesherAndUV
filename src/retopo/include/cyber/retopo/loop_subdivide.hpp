#pragma once

#include <cstddef>

#include "cyber/core/mesh.hpp"

// Triangle-to-triangle densification (Loop subdivision).
//
// Mesh::linearSubdivide is Catmull-Clark TOPOLOGY: it splits every n-gon into n
// quads, so a triangle mesh comes back as a quad mesh. That is the wrong tool
// when the caller wants a denser TRIANGLE mesh — which is what a displacement
// cage, a simulation mesh or a subdivision preview of triangle input needs.
// Loop subdivision is the triangle counterpart: 1 triangle -> 4 triangles.
namespace cyber::retopo {

// Which of the two jobs the caller is asking for. They differ ONLY in vertex
// placement — the 1-to-4 topology is identical — but the difference is the
// whole point, so it is never inferred.
enum class LoopSubdivideMode {
    // True Loop weights: edge points at 3/8-3/8-1/8-1/8 across the two adjacent
    // triangles and original vertices pulled toward their one-ring. The surface
    // converges to the Loop limit surface, so THE SHAPE CHANGES — a cube visibly
    // rounds off. Use it to smooth.
    Smooth,
    // Pure topological 1-to-4 split: every new vertex is an exact edge midpoint
    // and NO original vertex moves. The result interpolates the input surface
    // exactly (same silhouette, same volume, same planes), it just has 4x the
    // triangles. Use it when you want more polygons and the same shape.
    Linear,
};

// Why a subdivision was refused. Failure is always total: the caller's mesh is
// untouched and no partially subdivided result is produced.
enum class LoopSubdivideError {
    None = 0,
    // The mesh has no faces; there is nothing to densify.
    EmptyMesh,
    // A face has other than three sides. Loop subdivision is defined on
    // triangles only, and fan-triangulating on the caller's behalf would be a
    // silent topology edit, so we name the face and let the caller decide
    // (Mesh::triangulate() is the explicit opt-in).
    NonTriangleFace,
    // A triangle's edges could not be resolved from its corners, i.e. the mesh
    // violates its own structural invariants. Unreachable on any mesh
    // Mesh::validate() accepts; guarded because the alternative is an
    // out-of-range read on the id maps.
    MalformedFace,
};

// Static, human-readable name for an error code. Never null.
[[nodiscard]] const char* toString(LoopSubdivideError error);

struct LoopSubdivideResult {
    LoopSubdivideError error = LoopSubdivideError::None;
    // The subdivided mesh — meaningful only when `error` is None. A rebuilt
    // mesh: every vertex, edge and face id is reassigned.
    Mesh mesh;
    // Set for NonTriangleFace and MalformedFace: the first offending face (in
    // id order) and how many sides it actually has, so the caller can report
    // or fix it.
    FaceId offendingFace{};
    std::size_t offendingFaceSize = 0;

    [[nodiscard]] bool ok() const { return error == LoopSubdivideError::None; }
};

// Splits every triangle into four. Vertex, edge, face and corner attributes
// propagate the same way Mesh::linearSubdivide propagates them (originals
// copied, edge points averaged from their two endpoints, face attributes shared
// by the four children); feature flags carry from a parent edge onto both of
// its halves.
//
// BOUNDARIES: an open mesh keeps its boundary curve. Boundary edge points are
// plain midpoints and boundary vertices use the 1/8, 3/4, 1/8 cubic-B-spline
// curve rule, NOT the interior mask — running the interior mask on a boundary
// would drag the border inward and shrink the mesh every level.
[[nodiscard]] LoopSubdivideResult loopSubdivide(const Mesh& mesh, LoopSubdivideMode mode);

}  // namespace cyber::retopo
