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
//
// When `vertexAreas` is non-null it receives the mixed Voronoi area each
// vertex's estimate was normalized by (same indexing, 0 for dead vertices).
// That is the surface each value speaks for, and it is what curvatureScale()
// wants as its weights.
[[nodiscard]] std::vector<float> vertexMeanCurvature(const Mesh& mesh,
                                                     std::vector<float>* vertexAreas = nullptr);

// Robust normalization scale for a curvature field: the `percentile` (0..1)
// of |H| over live vertices. Returns 0 for an empty or perfectly flat field,
// which callers must treat as "nothing to normalize against".
//
// This is what BakeParams::curvatureRange = 0 (auto) resolves to. A percentile
// rather than the maximum keeps one pinched vertex from flattening the whole
// map to mid-gray.
[[nodiscard]] float curvatureScale(const std::vector<float>& curvature, float percentile = 0.95f);

// Same percentile, but each value counts for `weights[i]` instead of for one
// vertex -- pass the mixed areas from vertexMeanCurvature() to get a percentile
// over SURFACE rather than over vertices.
//
// The distinction decides the range on any mesh whose vertex density is uneven.
// A UV sphere piles a fifth of its vertices into the sliver fans around its two
// poles, which are a per-mille of its area; an unweighted percentile therefore
// reports the poles' curvature as if it were typical and normalizes the whole
// map against it. Weighting by area makes a region count for as much of the
// range as it covers of the model.
//
// `weights` must be parallel to `curvature`; any other size is ignored and the
// call degrades to the unweighted percentile.
[[nodiscard]] float curvatureScale(const std::vector<float>& curvature,
                                   const std::vector<float>& weights, float percentile = 0.95f);

}  // namespace cyber::bake
