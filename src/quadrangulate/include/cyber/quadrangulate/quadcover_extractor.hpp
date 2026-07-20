#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/quadrangulate.hpp"

// QuadCover-style seamless-UV isoline quad extractor (TASK F scaffold).
//
// Why this exists
// ---------------
// Our production integer extractor (quad_extract.cpp) reaches quads by a
// zero-diff *collapse* of the integer grid. That collapse quantises the grid and
// caps the irregular-vertex fraction at ~7-13% (QuadriFlow sits at 1-4%). The
// collapse paradigm is the ceiling — flip-repair, multi-res reads and T-junction
// cleanup all give only marginal gains (roadmap §6·H).
//
// The structurally different path is QuadCover: solve a *seamless* integer-grid
// parameterization (u,v) on the triangle mesh so that every integer isoline is
// continuous across triangle edges, then trace the integer isolines. Where a
// u-isoline crosses a v-isoline the crossing is transversal, so the extracted
// vertex is valence-4 BY CONSTRUCTION. Singularities appear only at genuine field
// cone points, not as a side effect of quantisation.
//
// This file is a COMPILABLE SKELETON only. makeQuadCoverQuadrangulator() builds
// and returns a valid IQuadrangulator whose quadrangulate() is a guarded no-op
// that reports "not implemented" (never corrupts the mesh). The real
// implementation is a multi-milestone project — see
// docs/quadcover-plan.md (to be written by the next engineer
// from the plan in the Task F return text) and the two collaborators below.
//
// Honest expectation (roadmap §6·F): benchmarking AutoRemesher's own QuadCover
// pipeline (Geogram quad_cover + isoline quadextractor.cpp) measured irregular
// 6-15% — NOT better than our integer extractor. Reaching 1-4% is dominated by
// the *solver* (frame field + seamless-UV quality), not the isoline tracer. This
// scaffold is the correct structure to host a better solver; it is not, by
// itself, a guaranteed win. Sequence the milestones so the solver decision (see
// the Task F return text) is validated before the extractor port is finished.
namespace cyber::remesh {

// -----------------------------------------------------------------------------
// Collaborator 1 (NOT IMPLEMENTED): the seamless integer-grid parameterization.
// -----------------------------------------------------------------------------
// The load-bearing dependency. Produces, for every triangle corner, a 2-D UV in
// a *seamless* integer grid: across each interior edge the two corner UVs of the
// shared vertices differ only by a grid symmetry (a 90-degree rotation about a
// lattice point + integer translation). This is what makes the isolines
// continuous. AutoRemesher gets it from Geogram's GEO::GlobalParam2d::quad_cover
// (parameterizer.cpp); we do NOT produce it yet. The SOLVER DECISION in the Task
// F return text weighs vendoring Geogram vs. an MIQ/QuadCover reimplementation
// vs. reusing the benchmark-harness build path.
struct SeamlessUv {
    // triangleUv[t] holds the 3 corner UVs of triangle t, in the same corner
    // order as the source mesh's triangle. Empty until a solver fills it.
    std::vector<std::array<Vec2, 3>> triangleUv;
    // True once a real seamless solve has populated triangleUv. The stub leaves
    // this false so the extractor can guard.
    bool valid = false;
};

// Placeholder for the solver seam. The real signature will also take the
// orientation/position field (see computePositionField in position_field.hpp)
// and the target edge length / per-face adaptive scaling. Returns an invalid
// SeamlessUv until a solver is wired in.
[[nodiscard]] SeamlessUv computeSeamlessUv(const Mesh& mesh, float targetEdgeLength);

// -----------------------------------------------------------------------------
// Collaborator 2 (NOT IMPLEMENTED): the isoline tracer / mesh extractor.
// -----------------------------------------------------------------------------
// Ports AutoRemesher's Qt-free, Geogram-free quadextractor.cpp to our Vec3/Mesh
// types. Pipeline: extractConnections (trace integer isolines per triangle and
// segment them at transversal crossings) -> extractEdges -> collapse* (short
// edges, degenerate triangles, single endpoints) -> extractMesh (orbit-walk the
// connection graph into oriented quads) -> fixHoles. Helpers to port alongside
// it: PositionKey (a 1e-5-quantised Vec3 map key for welding coincident cross
// points), Double (epsilon compare) and MeshSeparator (split into islands / build
// edge->face map). Returns quads as index lists into `outVertices`; empty until
// implemented.
struct IsolineQuadMesh {
    std::vector<Vec3> vertices;
    std::vector<std::vector<std::size_t>> quads;  // CCW corner indices per face
};
[[nodiscard]] IsolineQuadMesh extractIsolineQuads(const Mesh& mesh, const SeamlessUv& uv);

// -----------------------------------------------------------------------------
// The IQuadrangulator seam (scaffold — returns a working stub).
// -----------------------------------------------------------------------------
// Computes the seamless UV, traces integer isolines, and rewrites `mesh` in place
// with the transversal-crossing quad mesh. Feature edges are respected as hard
// grid constraints by the solve. SCAFFOLD STATUS: quadrangulate() currently
// leaves the mesh untouched and returns a failure Outcome with a "not
// implemented" reason, so the pipeline degrades cleanly rather than crashing.
std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations = 40);

}  // namespace cyber::remesh
