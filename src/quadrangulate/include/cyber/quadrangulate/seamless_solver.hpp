#pragma once

#include <cstddef>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/crossfield.hpp"

// Native QuadCover-style seamless integer-grid parameterizer (docs/native-miq-plan.md).
// The path to dropping the vendored Geogram quad_cover dependency: reuse our own 4-RoSy
// cross field, then a seamless Poisson solve. Built in milestones — this header exposes
// M1 (the frame-field setup: period jumps, singularities, and a cut graph that opens the
// surface to a disk). M2 (the seamless solve) consumes a SeamlessSetup.
namespace cyber::remesh {

// The pre-solve topology + field data a seamless parameterization needs.
struct SeamlessSetup {
    CrossField field;  // per-face 4-RoSy cross field (the frame the UV aligns to)

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
[[nodiscard]] SeamlessSetup buildSeamlessSetup(const Mesh& mesh, int iterations,
                                               accel::IBackend& backend);

// Euler characteristic V - E + F of the mesh cut open along `setup.isCutEdge`: each cut
// edge is split so the two sides no longer share it. A cut graph that opens a closed
// surface to a disk yields 1. Validation hook for M1 (and a guard for M2's flattening).
[[nodiscard]] int cutOpenEulerCharacteristic(const Mesh& mesh, const SeamlessSetup& setup);

// The relaxed seamless parameterization (Milestone 2). Solved on the mesh CUT OPEN along
// the cut graph (seam vertices duplicated), so the UV can jump by a grid symmetry across
// the seam — a closed surface cannot carry a globally continuous integer-grid UV. Per
// coordinate it is a cotangent Poisson solve (grad(u,v) matches the combed frame field
// scaled to `spacing`) via Conjugate Gradient on accel::spmv. Output is PER-CORNER UV
// (cornerUv[faceId] = the 3 corner UVs). This is the RELAXED solve — real-valued, so the
// seam translations are not yet integers (seamlessUvResidual > 0); integer rounding of
// the seams (M2c) drives the residual to 0.
struct Parameterization {
    std::vector<std::array<Vec2, 3>> cornerUv;  // per FaceId; empty array for dead faces
    int cutVertexCount = 0;   // vertices of the cut-open (disk) mesh the solve ran on
    int cgIterationsU = 0;
    int cgIterationsV = 0;
    bool valid = false;
};
[[nodiscard]] Parameterization solveParameterization(const Mesh& mesh, const SeamlessSetup& setup,
                                                     float spacing, accel::IBackend& backend);

}  // namespace cyber::remesh
