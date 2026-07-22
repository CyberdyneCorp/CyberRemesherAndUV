#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
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
    // The mesh the UV lives on. The solver (AutoRemesher via the M1 harness)
    // isotropically remeshes internally, so the UV is NOT on the input triangles —
    // it comes with its own vertices/triangles, and the extractor (M2) traces
    // isolines on THIS mesh, then the pipeline reprojects onto the source surface.
    std::vector<Vec3> vertices;
    std::vector<std::array<Index, 3>> triangles;
    // triangleUv[t] holds the 3 corner UVs of triangles[t], same corner order.
    std::vector<std::array<Vec2, 3>> triangleUv;
    // True once a real seamless solve has populated the fields. The stub / a failed
    // solve leaves this false so the extractor can guard.
    bool valid = false;
};

// Compute a seamless integer-grid UV for `mesh`. Milestone 1 obtains it out-of-process
// from AutoRemesher's Geogram quad_cover via the benchmark harness: the binary path is
// read from the CYBER_QUADCOVER_CLI environment variable (built by
// examples/reference/build_autoremesher.sh); if unset or the run fails, an INVALID
// SeamlessUv (valid == false) is returned so callers degrade cleanly. A later milestone
// replaces the subprocess with a vendored/native solver (see docs/quadcover-plan.md).
// `harnessScaling` is passed to the harness (-s): it controls the isotropic-remesh
// density that drives the seamless-UV grid resolution and thus the extracted quad
// count (lower -> denser -> more quads, ~1/s^2). The quad-cover quadrangulator sweeps
// it in a short closed loop to hit the requested count. CYBER_QC_SCALING overrides it.
//
// `harnessAdaptivity` is passed to the harness (-a): the frame-field gradient
// adaptivity. 0.0 is a uniform field (fewest singularities, cleanest topology — the
// default); higher packs quads into high-curvature regions (better surface fidelity
// per polygon, but more singularities). CYBER_QC_ADAPT overrides it.
[[nodiscard]] SeamlessUv computeSeamlessUv(const Mesh& mesh, float targetEdgeLength,
                                           float harnessScaling = 0.5f,
                                           float harnessAdaptivity = 0.0f,
                                           const CancelToken* cancel = nullptr);

// Native seamless integer-grid parameterizer (docs/native-miq-plan.md) — the path to
// dropping the vendored-Geogram dependency entirely. QuadCover-style: reuse our own
// 4-RoSy frame field (computePositionField) + a cut graph, solve the seamless Poisson
// system with Conjugate Gradient on accel::spmv, round seam jumps with MinCostFlow.
// M0 SCAFFOLD: returns an INVALID SeamlessUv (valid == false) until the solve (M2)
// lands, so computeSeamlessUv falls through to the vendored/harness path. Opt-in via
// CYBER_QC_NATIVE so it never affects the shipped path before it validates.
[[nodiscard]] SeamlessUv computeSeamlessUvNative(const Mesh& mesh, float targetEdgeLength,
                                                 float adaptivity = 0.0f, float spacingScale = 1.0f,
                                                 const CancelToken* cancel = nullptr);

// Max integer-jump residual of a seamless UV across its interior edges: for each edge
// shared by two triangles, the grid symmetry mapping one triangle's shared-vertex UVs
// to the other's must have an INTEGER translation. 0 == perfectly seamless. A
// validation hook (M1); returns 0 for an empty/invalid UV.
[[nodiscard]] double seamlessUvResidual(const SeamlessUv& uv);

// Fraction of interior (exactly-two-face) edges whose dihedral angle exceeds
// `dihedralDegrees` — a CAD discriminator. High on sharp/crease-heavy parts
// (fandisk ~4%), near zero on smooth organic meshes. computeSeamlessUv uses it
// to route crease-heavy meshes to the feature-aware native seamless solver
// (squarer quads on CAD) while smooth meshes keep the vendored Geogram path.
// Const / non-mutating (unlike Mesh::tagFeatureEdges).
[[nodiscard]] float creaseEdgeFraction(const Mesh& mesh, float dihedralDegrees);

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

// Post-extraction cap elimination. The isoline tracer leaves a few percent of non-quad
// "cap" faces (triangles / pentagons / hexagons) at cone and boundary regions; under the
// pipeline's pure-quad Catmull-Clark subdivision each becomes a valence-n fan-centre
// irregular. This pass re-partitions those caps into quads over the SAME vertex set
// (pairing adjacent odd faces, splitting even n-gons), keeping the mesh watertight and
// never increasing the final irregular count. `faces` is edited in place.
void eliminateNonQuadCaps(std::vector<Vec3>& vertices,
                          std::vector<std::vector<std::size_t>>& faces);

// -----------------------------------------------------------------------------
// The IQuadrangulator seam (scaffold — returns a working stub).
// -----------------------------------------------------------------------------
// Computes the seamless UV, traces integer isolines, and rewrites `mesh` in place
// with the transversal-crossing quad mesh. Feature edges are respected as hard
// grid constraints by the solve. SCAFFOLD STATUS: quadrangulate() currently
// leaves the mesh untouched and returns a failure Outcome with a "not
// implemented" reason, so the pipeline degrades cleanly rather than crashing.
// `adaptivity` (0.0 = uniform, the cleanest topology; up to 1.0 = fully
// curvature-adaptive sizing) is forwarded to the seamless-UV solve as the frame-field
// gradient adaptivity. The pipeline passes the run's adaptivity through here.
std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations = 40,
                                                             float adaptivity = 0.0f);

// Whether a seamless-UV solver is available for the quad-cover method: true when the
// in-process solver is linked (built with -DCYBER_WITH_QUADCOVER=ON) or the
// CYBER_QUADCOVER_CLI harness binary is configured. Callers making quad-cover the
// default use this to fall back to the field-aligned quadrangulator when neither is
// present, so a build without the solver still produces output.
[[nodiscard]] bool quadCoverAvailable();

}  // namespace cyber::remesh
