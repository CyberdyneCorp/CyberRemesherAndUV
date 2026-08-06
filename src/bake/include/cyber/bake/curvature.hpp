#pragma once

#include <vector>

#include "cyber/core/mesh.hpp"

// Discrete curvature estimation on a Target mesh (surface-baking spec,
// "Curvature and cavity maps"). Kept separate from bake.cpp so the estimator is
// testable against analytic shapes independently of ray casting.
namespace cyber::bake {

// Per-vertex signed mean curvature, indexed by VertexId::value with
// mesh.vertexCapacity() entries (dead vertices read 0). Units are 1/length:
// a sphere of radius r reads 1/r everywhere. Convex regions are POSITIVE,
// concave regions negative, flat regions zero.
//
// Meyer et al. 2003 discrete operator: the mean-curvature normal
//   K(v) = 1/(2*A_mixed) * sum_j (cot a_ij + cot b_ij) * (p_v - p_j)
// projected onto the vertex normal, halved (K = 2*H*n). n-gon faces are
// fan-triangulated on their corners, matching how the BVH triangulates them, so
// the estimate is consistent with what the bake rays actually hit.
//
// The cotangent formula has no meaning on a boundary 1-ring (the missing wedge
// is not a flat wedge), so boundary vertices instead take the mean of their
// interior neighbours -- an open rim reads as a continuation of the surface
// behind it rather than as a spurious crease.
[[nodiscard]] std::vector<float> vertexMeanCurvature(const Mesh& mesh);

// Robust normalization scale for a curvature field: the `percentile` (0..1)
// of |H| over live vertices. Returns 0 for an empty or perfectly flat field,
// which callers must treat as "nothing to normalize against".
//
// This is what BakeParams::curvatureRange = 0 (auto) resolves to. A percentile
// rather than the maximum keeps one pinched vertex from flattening the whole
// map to mid-gray.
[[nodiscard]] float curvatureScale(const std::vector<float>& curvature, float percentile = 0.95f);

}  // namespace cyber::bake
