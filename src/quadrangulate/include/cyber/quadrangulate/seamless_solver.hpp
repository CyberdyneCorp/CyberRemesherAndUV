#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/guidance.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/quadrangulate/crossfield.hpp"

// Native QuadCover-style seamless integer-grid parameterizer (docs/native-miq-plan.md).
// The path to dropping the vendored Geogram quad_cover dependency: reuse our own 4-RoSy
// cross field, then a seamless Poisson solve. Built in milestones — this header exposes
// M1 (the frame-field setup: period jumps, singularities, and a cut graph that opens the
// surface to a disk). M2 (the seamless solve) consumes a SeamlessSetup.
namespace cyber::remesh {

// The pre-solve topology + field data a seamless parameterization needs.
struct SeamlessSetup {
    CrossField field;

    // Feature-binding mode (the shipped feature-pinning lever): feature edges
    // become hard seams with integer-pinned crease isolines, period jumps are
    // computed across creases, and the combed targets/rho use the angle()
    // branch convention. Off reproduces the historical knife-edge-only
    // behavior bit-exactly.
    bool featureBinding = false;  // per-face 4-RoSy cross field (the frame the UV aligns to)

    // Per-edge integer period jump r in {0,1,2,3}: the 90-degree rotation that aligns
    // edgeFaces[1]'s cross to edgeFaces[0]'s across that edge (0 for boundary/feature/
    // dead edges). Indexed by EdgeId value.
    std::vector<int> periodJump;

    // Per-vertex cross-field index (0 = regular). Interior cone points where the field
    // winds; by Poincare-Hopf sum(index) == 4 * eulerCharacteristic. Indexed by VertexId.
    std::vector<int> singularityIndex;

    // Per-edge flag: this edge is on the cut graph (a tree through the singular vertices
    // that opens the surface into a disk so it can be flattened). Indexed by EdgeId.
    std::vector<bool> isCutEdge;

    bool valid = false;

    [[nodiscard]] std::size_t singularityCount() const;
    // Sum of singularity indices (== 4 * eulerCharacteristic for a valid field).
    [[nodiscard]] int totalIndex() const;
};

// Build the M1 setup: cross field (via computeCrossField, `iterations` smoothing sweeps),
// per-edge period jumps, per-vertex singularity indices, and a cut graph. Returns
// valid == false only for an empty/degenerate mesh.
//
// `creaseAlignSupport` (optional, indexed by EdgeId): resolution-aware crease-pin gating,
// forwarded to computeCrossField and to the dipole-annihilation barrier — see
// crossfield.hpp. Null keeps every crease pin (historical behavior).
//
// `guidance` (optional) carries user-drawn flow guides into the cross-field
// solve as a soft alignment constraint — see crossfield.hpp. Null leaves the
// field build textually unchanged.
[[nodiscard]] SeamlessSetup buildSeamlessSetup(
    const Mesh& mesh, int iterations, accel::IBackend& backend, bool featureBinding = false,
    const std::vector<char>* creaseAlignSupport = nullptr, const GuidanceField* guidance = nullptr);

// Euler characteristic V - E + F of the mesh cut open along `setup.isCutEdge`: each cut
// edge is split so the two sides no longer share it. A cut graph that opens a closed
// surface to a disk yields 1. Validation hook for M1 (and a guard for M2's flattening).
[[nodiscard]] int cutOpenEulerCharacteristic(const Mesh& mesh, const SeamlessSetup& setup);

// The seamless integer-grid parameterization (Milestone 2). Solved on the mesh CUT OPEN
// along the cut graph (seam vertices duplicated), so the UV jumps by a grid symmetry
// (uv_B = R^rho uv_A + integer t) across the seam — a closed surface cannot carry a globally
// continuous integer-grid UV. Two phases:
//   (M2a) a cotangent Poisson solve per coordinate (grad(u,v) matches the combed frame field
//         scaled to `spacing`) via Conjugate Gradient on accel::spmv — the RELAXED seam;
//   (M2c) a constrained re-solve: the seam transitions and gauge become HARD homogeneous
//         constraints once the integer translations are promoted to variables; exact ±1
//         Gauss-Jordan reduces them to independent DOF (reconciling branch-point holonomy),
//         the reduced Dirichlet energy is CG-solved via accel::spmv, and the integer
//         translations are rounded greedily. This turns the non-rigid relaxed seam into a
//         rigid integer grid so seamlessUvResidual drops to ~0.
// Output is PER-CORNER UV (cornerUv[faceId] = the 3 corner UVs). The integer phase uses a
// sparse constraint elimination (no dense dual, no seam cap), so it scales to meshes with
// hundreds of singularities (e.g. spot: 92 cones).
struct Parameterization {
    std::vector<std::array<Vec2, 3>> cornerUv;  // per FaceId; empty array for dead faces
    int cutVertexCount = 0;  // vertices of the cut-open (disk) mesh the solve ran on
    int cgIterationsU = 0;
    int cgIterationsV = 0;
    bool valid = false;
};

// Opaque cache for the direct (sparse-Cholesky) solve path, CYBER_QC_DIRECT
// (docs/ROADMAP.md perf entry). The pinned Poisson operator and the reduced
// integer-phase operator depend only on (mesh, setup) — never on `spacing` —
// so their factorizations are computed once and reused by the calibration
// probe and every calibration attempt (only the RHS re-scales). Callers that
// re-solve the SAME mesh+setup at different spacings pass the same cache (the
// quad-cover NativeSolveContext carries one); a null cache just factors per
// call. Never consulted when the CYBER_QC_NO_DIRECT kill switch is set.
class SeamlessSolveCacheImpl;
struct SeamlessSolveCache {
    std::shared_ptr<SeamlessSolveCacheImpl> impl;
};

//
// `density` (optional) is the painted sizing multiplier: the per-face target
// grid spacing becomes spacing / sqrt(density), i.e. the RHS is scaled per
// face. It touches the RHS ONLY — exactly like `spacing` itself — so the
// cached factorizations in `cache` stay valid across density changes.
[[nodiscard]] Parameterization solveParameterization(const Mesh& mesh, const SeamlessSetup& setup,
                                                     float spacing, accel::IBackend& backend,
                                                     const CancelToken* cancel = nullptr,
                                                     SeamlessSolveCache* cache = nullptr,
                                                     const GuidanceField* density = nullptr);

// Relaxed-only calibration probe: runs solveParameterization's assembly + the initial
// relaxed Poisson solve at `spacing` (no ARAP polish, no integer phase) and returns the
// UV grid-cell count — the sum of |UV triangle areas|, ~1 extracted quad candidate per
// unit cell. The relaxed phase already fixes the cell area (ARAP keeps the target frame
// norm and integer rounding only shifts sub-cell translations), so this predicts the
// extraction count at a fraction of the full solve's cost. Returns <= 0 for a
// degenerate setup/mesh or a cancelled solve. Consumed by the quad-cover
// quadrangulator's initial-scaling probe (quadcover_extractor.cpp).
[[nodiscard]] double relaxedCellArea(const Mesh& mesh, const SeamlessSetup& setup, float spacing,
                                     accel::IBackend& backend, const CancelToken* cancel = nullptr,
                                     SeamlessSolveCache* cache = nullptr,
                                     const GuidanceField* density = nullptr);

}  // namespace cyber::remesh
