#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/quadrangulate/seamless_solver.hpp"

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

// Attempt-invariant work of the NATIVE seamless solve, shareable across the
// quadrangulator's density-calibration attempts: the isotropic pre-remesh, the
// cross field + cut graph (SeamlessSetup), and the feature-binding decision.
// None of it depends on the calibration `scaling` — only solveParameterization
// consumes the grid spacing — so computing it once and re-solving at different
// spacings is byte-identical to recomputing it per attempt (it is a
// deterministic function of the input mesh / target edge length / adaptivity /
// feature threshold). Pass a context to computeSeamlessUv(Native); a null
// context (the default) recomputes everything per call, exactly as before.
struct NativeSolveContext {
    bool ready = false;   // prepared work/setup below are valid and reusable
    Mesh work;            // isotropic-remeshed, feature-tagged solve mesh
    SeamlessSetup setup;  // cross field + period jumps + cut graph on `work`
    bool featureBinding = false;
    // Resolution-aware feature demotion (kill switch CYBER_QC_NO_FEATURE_DEMOTE): per-edge
    // flag on `work` — 1 where a crease dihedral traces the ORIGINAL mesh's resolvable sharp
    // network and may pin the cross field, 0 for sub-resolution wrinkle dihedrals. Empty when
    // demotion is off (historical: every crease pins). See prepareNativeSolve.
    std::vector<char> creaseAlignSupport;
    // Direct-solver factorizations (CYBER_QC_DIRECT): the solve operators are
    // spacing-invariant, so the probe and every calibration attempt share them.
    SeamlessSolveCache solveCache;
    // User-drawn guidance (flow guides + painted density). Non-null FORCES the
    // native route: the vendored Geogram quad_cover builds its own frame field
    // from its own single scalar density inside sources we do not patch, so it
    // has no hook for either input. Set it before the first call; the isotropic
    // pre-remesh, the cross field and the seamless RHS all read it from here.
    const GuidanceField* guidance = nullptr;
    // Empty while the guidance is honored; otherwise the user-facing reason the
    // guided island ended up on a path that could not honor it — the vendored
    // Geogram solve, whether because native declined or because
    // CYBER_QC_NO_NATIVE disabled it. A reason, not a bool, because the two
    // cases need different remedies (one is a solver failure, the other is an
    // env var the user set). Read back by the quadrangulator and reported per
    // island — never swallowed.
    std::string guidanceUnhonoredReason;
    // Topology-layout options for this solve (ZRemesher). `capture` FORCES the
    // native route for the same reason guidance does: the layout is traced from
    // the native seamless map, and the vendored Geogram quad_cover produces no
    // T-mesh at all — routing there would silently return no layout.
    SeamlessLayoutOptions layout;
    // Which cross field this solve builds from. `Auto` keeps the shipped
    // choice; the candidate-selection path names one explicitly so it can solve
    // both and compare without a process-global switch.
    CrossFieldSource fieldSource = CrossFieldSource::Auto;
    // Topology-guide accounting, read back for the run report. A requested
    // guide that was not honoured is named, never dropped.
    std::size_t topologyGuidesRequested = 0;
    std::size_t topologyGuidesHonored = 0;
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
// `featureDegrees` is the dihedral threshold binding sharp edges into the NATIVE
// solve (hard seams + feature-pinned integer isolines); it never reaches the
// vendored path (see ROADMAP c1b — vendored thresholds are a count artifact).
// Default keeps the historical 40 (knife edges only); the CLI forwards its
// documented --sharp-edge (default 90). CYBER_QC_FEATURE_DEG still overrides.
// `ctx` (optional) shares the attempt-invariant native-solve work across calls —
// see NativeSolveContext; it is only touched when the native path runs.
[[nodiscard]] SeamlessUv computeSeamlessUv(const Mesh& mesh, float targetEdgeLength,
                                           float harnessScaling = 0.5f,
                                           float harnessAdaptivity = 0.0f,
                                           const CancelToken* cancel = nullptr,
                                           float featureDegrees = 40.0f,
                                           NativeSolveContext* ctx = nullptr);

// Native seamless integer-grid parameterizer (docs/native-miq-plan.md) — the path to
// dropping the vendored-Geogram dependency entirely. QuadCover-style: reuse our own
// 4-RoSy frame field (computePositionField) + a cut graph, solve the seamless Poisson
// system with Conjugate Gradient on accel::spmv, round seam jumps with MinCostFlow.
// M0 SCAFFOLD: returns an INVALID SeamlessUv (valid == false) until the solve (M2)
// lands, so computeSeamlessUv falls through to the vendored/harness path. Opt-in via
// CYBER_QC_NATIVE so it never affects the shipped path before it validates.
// `ctx` (optional): when it is ready, the isotropic remesh + field + setup are
// reused instead of recomputed; when it is not, they are computed and stored so
// the next call at a different spacing skips them (byte-identical either way).
[[nodiscard]] SeamlessUv computeSeamlessUvNative(const Mesh& mesh, float targetEdgeLength,
                                                 float adaptivity = 0.0f, float spacingScale = 1.0f,
                                                 const CancelToken* cancel = nullptr,
                                                 float featureDegrees = 40.0f,
                                                 NativeSolveContext* ctx = nullptr);

// Calibration probe for the NATIVE solve: prepares `ctx` (isotropic remesh + field +
// cut setup) when it is not ready yet, then runs ONLY the relaxed Poisson phase of the
// seamless solve at grid spacing == targetEdgeLength and returns the UV grid-cell count
// (sum of |UV triangle areas|; ~1 extracted quad candidate per unit cell, see
// relaxedCellArea). The quad-cover quadrangulator uses it to predict the initial
// calibration scaling so most meshes land in the acceptance window in ONE full solve.
// Returns <= 0 when any stage declines — callers keep the historical hardcoded start.
[[nodiscard]] double nativeRelaxedCellArea(const Mesh& mesh, float targetEdgeLength,
                                           float adaptivity, float featureDegrees,
                                           const CancelToken* cancel, NativeSolveContext& ctx);

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
// `holeFillMaxBoundary` is the run's hole-fill policy (remeshing-parameters spec):
// boundary loops with at most this many edges are closed with quads during
// extraction, longer ones are left open, and a value below 3 disables filling
// entirely. It must be threaded in here rather than applied downstream: the
// pipeline's post-pass can only fill loops that SURVIVE extraction, so a
// hard-coded limit here silently overrides whatever the caller asked for.
[[nodiscard]] IsolineQuadMesh extractIsolineQuads(const Mesh& mesh, const SeamlessUv& uv,
                                                  int holeFillMaxBoundary = 64);

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
// `holeFillMaxBoundary` forwards the run's hole-fill policy to extraction (see
// extractIsolineQuads above); the default matches RemeshParams so a caller that
// does not care keeps the documented behaviour.
// Guidance (IQuadrangulator::acceptGuidance) is honored on the NATIVE seamless
// route only, which supplying it therefore forces — see NativeSolveContext.
// That is a measured quality trade on builds where the vendored Geogram solve is
// available (it wins on smooth organic meshes); it is documented in
// docs/flow-guides.md rather than hidden.
std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations = 40,
                                                             float adaptivity = 0.0f,
                                                             int holeFillMaxBoundary = 64,
                                                             float featureDegrees = 40.0f);

// ZRemesher-class retopology (openspec/changes/add-zremesher-retopology,
// docs/zremesher-plan.md). Structurally the quad-cover path — same cross field,
// same seamless solve, same isoline extraction — with the explicit
// TopologyLayout stage turned on and the tracing options the LAYOUT wants
// rather than the ones the shipped quantizer's guided rounding wants.
//
// Concretely that means boundary chains on, so a separatrix reaching an open
// boundary terminates there and becomes a boundary arc instead of being
// abandoned (measured on the Stanford bunny: abandoned launches 20 -> 1,
// contained regions 47 -> 39), and fold repair on. Neither is safe to flip for
// `quad-cover` — they reshape the flow its guided rounding is tuned against —
// which is exactly why the layout needs its own method rather than another
// environment variable on the old one.
//
// It always routes NATIVE: the layout is traced from the native seamless map,
// and the vendored Geogram solve produces no T-mesh to trace.
// How much work the method may spend finding a good answer.
//
// Fast solves one predicted path. Best solves both cross-field candidates and
// keeps the one that scores better — which is what the roadmap's open question
// asks for: no static "organic vs CAD" threshold picks the right field for
// every model, because rocker-arm prefers one and spot and cheburashka the
// other while their crease fractions interleave. Measuring both is the answer;
// it costs a second solve.
enum class RemeshQualityMode : std::uint8_t {
    Fast,
    Best,
};

struct ZRemesherOptions {
    RemeshQualityMode quality = RemeshQualityMode::Fast;
    int fieldIterations = 40;
    float adaptivity = 0.0f;
    int holeFillMaxBoundary = 64;
    float featureDegrees = 40.0f;
    // Terminate separatrices on open boundaries instead of abandoning them.
    bool boundaryChains = true;
    // Recover fold-damaged node rotations by feasible-range projection.
    bool foldRepair = true;
    // Size the solve substrate from the unified sizing field (thin-feature risk
    // on top of the curvature adaptivity and painted density the isotropic
    // stage already derives).
    //
    // OFF: measured to make thin-feature survival WORSE, which is the one thing
    // it exists to improve. On the thin-feature fixtures
    // (examples/23_thin_features.py) it takes survival from 2 of 3 to 1 of 3 —
    // the fin survives without it and collapses with it — and on the corpus it
    // adds cones (fandisk 93 -> 104, cheburashka 92 -> 106) for a slightly
    // BETTER mean placement, i.e. more cones each sitting a little better.
    // Refining the substrate does not stop the extraction bridging a thin gap;
    // it just spends the budget getting there. Kept behind the flag so the
    // field has a consumer to be re-measured through once the extraction side
    // can act on it.
    bool unifiedSizing = false;
};
std::unique_ptr<IQuadrangulator> makeZRemesherQuadrangulator(const ZRemesherOptions& options = {});

// Whether a seamless-UV solver is available for the quad-cover method: true when the
// in-process solver is linked (built with -DCYBER_WITH_QUADCOVER=ON) or the
// CYBER_QUADCOVER_CLI harness binary is configured. Callers making quad-cover the
// default use this to fall back to the field-aligned quadrangulator when neither is
// present, so a build without the solver still produces output.
[[nodiscard]] bool quadCoverAvailable();

// Which seamless-UV solvers THIS BUILD can route an island to. The native
// solver is compiled in unconditionally; the vendored Geogram quad_cover solve
// is linked only with -DCYBER_WITH_QUADCOVER=ON, and it is the route most
// meshes actually take, so two binaries of the same version can produce
// different quads. Reported by `cyberremesh --version` so a quality report
// says which one produced it. Stable, machine-readable, no whitespace.
[[nodiscard]] std::string quadCoverSolverBuild();

}  // namespace cyber::remesh
