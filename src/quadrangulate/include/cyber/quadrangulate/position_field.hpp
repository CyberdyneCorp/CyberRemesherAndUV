#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/quadrangulate.hpp"

// Instant-Meshes-style position-field quadrangulator: an MIT-clean
// reimplementation of the ideas in "Instant Field-Aligned Meshes" (Jakob,
// Tarini, Panozzo, Sorkine-Hornung, SIGGRAPH Asia 2015). Unlike triangle
// pairing, it builds a per-vertex 4-RoSy ORIENTATION field and a POSITION
// (translation) field snapped to a lattice of the target edge length, then
// extracts quads by collapsing vertices that share a lattice cell and walking
// the resulting grid graph. This yields quads that follow the field with fewer,
// better-placed singularities. No mixed-integer solver is used (so none of
// AutoRemesher's GPL QuadCover/CoMISo machinery is involved).
namespace cyber::remesh {

// Per-vertex tangent frame + 4-RoSy orientation direction (`q`, a unit vector in
// the vertex tangent plane) + position-field point (`o`, a point near the vertex
// on the field lattice). Exposed for testing and for the extractor.
struct PositionField {
    std::vector<Vec3> normal;    // per-vertex unit normal (area-weighted)
    std::vector<Vec3> q;         // per-vertex orientation direction (tangent plane)
    std::vector<Vec3> o;         // per-vertex lattice position (near the vertex)
    std::vector<bool> valid;     // false for dead/isolated vertices
    float spacing = 1.0f;        // target lattice spacing (edge length)

    [[nodiscard]] std::size_t size() const { return q.size(); }
    // The orthogonal field direction q_perp = normal x q.
    [[nodiscard]] Vec3 qPerp(std::size_t i) const;
};

// Computes the orientation + position fields on a triangle mesh. `spacing` is
// the target quad edge length; `iterations` smoothing sweeps for each field.
// The orientation field is a smooth, feature-aligned 4-RoSy field (validated);
// the position field is the continuous translation field that a quad extractor
// rounds to the lattice.
//
// NOTE: the quad *extraction* from these fields (global integer-coordinate
// assignment via spanning-tree integration with per-edge rotation/translation
// bookkeeping, then robust face tracing) is deliberately not implemented here —
// a naive lattice-collapse over-merges a continuous field. The field-aligned
// maximum-matching quadrangulator remains the production quadrangulator; this
// module is the field foundation for a future Instant-Meshes-style extractor.
[[nodiscard]] PositionField computePositionField(const Mesh& mesh, float spacing, int iterations);

// Extracts a quad-dominant mesh from the fields (Instant-Meshes mesh
// extraction: lattice-cell collapse + local face tracing — see quad_extract.cpp
// and THIRD_PARTY_NOTICES.md). Returns a fresh mesh; the input is not modified.
[[nodiscard]] Mesh extractQuadMesh(const Mesh& mesh, const PositionField& field);

// Diagnostic for the collapse stage (Stage A): number of lattice-cell nodes and
// undirected lattice edges the collapse produces. Used by tests to confirm the
// collapse yields ~area/spacing^2 cells rather than over-merging.
struct CollapsedGraphStats {
    std::size_t nodes = 0;
    std::size_t latticeEdges = 0;
};
[[nodiscard]] CollapsedGraphStats debugCollapse(const Mesh& mesh, const PositionField& field);

}  // namespace cyber::remesh
