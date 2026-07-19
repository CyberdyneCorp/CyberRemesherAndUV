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
    std::vector<float> scale;    // per-vertex lattice-spacing multiplier (1 = uniform)
    float spacing = 1.0f;        // base lattice spacing; local spacing = spacing * scale[i]

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

// Position-field quadrangulator (IQuadrangulator seam): computes the orientation
// + position fields and extracts a quad mesh (Instant-Meshes-style), replacing
// the triangle mesh wholesale. Its advantage over the triangle-pairing
// quadrangulators is field-aligned, uniform-grid edge flow. Experimental —
// exercise via the pipeline's quadrangulator injection. `iterations` is the
// field-smoothing sweep count per multiresolution level.
std::unique_ptr<IQuadrangulator> makeInstantMeshesQuadrangulator(int iterations = 25);

// Diagnostic for the collapse stage (Stage A): number of lattice-cell nodes and
// undirected lattice edges the collapse produces. Used by tests to confirm the
// collapse yields ~area/spacing^2 cells rather than over-merging.
struct CollapsedGraphStats {
    std::size_t nodes = 0;
    std::size_t latticeEdges = 0;
};
[[nodiscard]] CollapsedGraphStats debugCollapse(const Mesh& mesh, const PositionField& field);

// Integer-parametrization rewrite, Milestone 1 (see
// docs/integer-parametrization-plan.md). Assigns each vertex a global integer
// 2-D coordinate by spanning-tree integration of the field connection (per-edge
// 4-RoSy rotation + integer translation), then measures how well the mesh's
// independent loops close. Small/sparse holonomy defects mean the field is
// consistent enough for the Stage-2 integer solve; large/dense defects mean the
// orientation field must be strengthened (Phase 5) first. Pure measurement — it
// does not modify the mesh and is not yet on the production path.
struct IntegerConsistency {
    std::size_t vertices = 0;      // vertices reached by the spanning tree
    std::size_t loopEdges = 0;     // non-tree (independent-loop) interior edges tested
    std::size_t rotSingular = 0;   // loop edges where the orientation index disagrees
    std::size_t transDefect = 0;   // loop edges whose integer translation defect != 0
    double meanDefect = 0.0;       // mean L1 translation defect over loop edges
    double closedFraction = 0.0;   // fraction of loop edges with zero defect (higher = better)
};
[[nodiscard]] IntegerConsistency measureIntegerConsistency(const Mesh& mesh,
                                                           const PositionField& field);

// Self-test for the Stage-2 min-cost max-flow engine (the solver that will
// balance per-face coordinate divergences in the integer optimization). Returns
// true iff it reproduces known-answer flow problems. A validation hook while the
// integer solve is built; not on the production path.
[[nodiscard]] bool debugMinCostFlow();

// QuadriFlow's per-face joint-alignment position-singularity count: the number
// of triangles whose summed integer edge-diffs (in the face's jointly-aligned
// cross frame) are nonzero. These are the true position singularities the
// Stage-2 integer solve must place; everything else the flow drives to zero.
// A developable surface has ~none. Diagnostic while the solve is built.
[[nodiscard]] std::size_t debugPositionSingularities(const Mesh& mesh, const PositionField& field);

// Stage-2 integer solve (single-level, closed genus-0 mesh). Builds the edge-diff
// / face-orientation constraint graph, runs the min-cost flow to cancel the
// position-field divergences, and reports before/after. `residualMismatch` must
// be 0 (checkpoint A: the residual recomputed from edge_diff/orient matches the
// position singularities — proves the constraint setup). `postSingular` should be
// far below `preSingular` (the flow removed the spurious defects, leaving only
// the true singularities). Diagnostic while the solve is wired into extraction.
struct IntegerSolveStats {
    std::size_t faces = 0;
    std::size_t preSingular = 0;
    std::size_t residualMismatch = 0;
    std::size_t postSingular = 0;
    int flow = 0;
    int supply = 0;
};
[[nodiscard]] IntegerSolveStats debugIntegerSolve(const Mesh& mesh, const PositionField& field);

// Milestone 3, stage (a): edge subdivision. Runs the integer solve then splits
// every triangle edge whose integer jump spans more than one grid cell, so each
// surviving edge spans <= 1 cell (QuadriFlow's subdivide_edgeDiff). `maxDiff` MUST
// be <= 1 after — the invariant the collapse + quad extraction relies on.
// Diagnostic while extraction is wired in.
struct SubdivideStats {
    std::size_t trisBefore = 0;
    std::size_t trisAfter = 0;
    std::size_t vertsAfter = 0;
    int maxDiff = 0;
};
[[nodiscard]] SubdivideStats debugSubdivide(const Mesh& mesh, const PositionField& field);

// Milestone 3: the integer-parametrization quad extractor. Runs the Stage-2 solve,
// subdivides to unit cells, then collapses zero-diff edges into grid vertices and
// pairs the two triangles across each grid-cell diagonal into a quad (QuadriFlow
// AdvancedExtractQuad / BuildTriangleManifold, single-level). Unlike the local
// collapse-and-walk extractor, singularities appear only where the corrected
// integer grid genuinely demands them, so the output is mostly valence-4. Returns
// a fresh mesh; the input is not modified.
[[nodiscard]] Mesh extractIntegerQuadMesh(const Mesh& mesh, const PositionField& field);

// Diagnostic for the integer extractor: output quad / vertex counts, irregular
// (valence != 4) vertices, non-quad faces, and boundary (hole) edges. Used by
// tests to confirm the extraction is watertight and mostly valence-4.
struct IntegerExtractStats {
    std::size_t quads = 0;
    std::size_t verts = 0;
    std::size_t irregular = 0;
    std::size_t nonQuad = 0;
    std::size_t boundaryEdges = 0;
};
[[nodiscard]] IntegerExtractStats debugIntegerExtract(const Mesh& mesh, const PositionField& field);

}  // namespace cyber::remesh
