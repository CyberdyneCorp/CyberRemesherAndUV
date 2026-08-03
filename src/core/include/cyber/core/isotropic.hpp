#pragma once

#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/reference_surface.hpp"
#include "cyber/core/region_solve.hpp"

namespace cyber::remesh {

// Adaptive isotropic remeshing of a triangle mesh (remeshing-pipeline spec,
// "Feature-preserving isotropic stage"): per iteration — split long edges,
// collapse short edges, flip toward valence 6, tangentially smooth, project
// back to the reference surface. Edge lengths converge to
// [4/5, 4/3] * (adaptivity-scaled) target.
//
// Feature preservation: vertices on tagged feature edges are never
// collapsed, never smoothed off the feature and never projected; edge flips
// never flip a feature edge. Tag features (Mesh::tagFeatureEdges) before
// calling.
//
// Feature tagging does NOT, however, stop a SPLIT: SplitPass is the only pass
// here that inserts a vertex, and it has no feature or boundary guard. That is
// harmless for a sharp crease (the inserted vertex stays on the crease) but not
// for a region solve, where the prescribed boundary's EDGE SET is part of the
// contract. `region` is what makes splits respect it — see region_solve.hpp.
struct IsotropicOptions {
    float targetEdgeLength = 0.0f;  // required, > 0
    int iterations = 3;
    float adaptivity = 0.0f;           // 0 = uniform; curvature-adaptive otherwise
    float smoothNormalDegrees = 0.0f;  // 0 = flat projection; > 0 = PN-triangle smoothing
    // Null (the default) = whole-mesh remesh, byte-for-byte the previous
    // behaviour. Non-null = region solve; only SplitPass consults it.
    const RegionSolve* region = nullptr;
    // AUTHORED per-vertex density scales, indexed by VertexId::value
    // (add-weave-density-radial-symmetry): the density BRUSH. Same units as the
    // curvature-derived scales this stage already carries — a MULTIPLIER on the
    // target edge length, so > 1 is coarser and < 1 is finer.
    //
    // COMPOSITION RULE, decided before the code and asserted by test: the authored
    // scale is MULTIPLIED into the curvature-derived one, then the product is clamped
    // to the same [0.3, 3.0] band curvature alone is clamped to.
    //
    //  * Multiplication because both are multipliers on the same quantity, so it
    //    composes them without either needing to know about the other — and because
    //    it is COMMUTATIVE, which is what makes the result independent of the order
    //    the two were applied, a property the spec requires.
    //  * NOT override: discarding curvature where the artist painted coarse would
    //    throw away detail preservation the artist did not ask to lose.
    //  * Clamped to the SAME band, not a wider one, because the split/collapse
    //    thresholds are tuned against [0.3, 3.0]; letting a product reach 9.0 risks
    //    exactly the compounding runaway documented at kScaleAttribute (a 100x
    //    face-count explosion). An authored 3.0 over a curvature 3.0 therefore
    //    yields 3.0 — honoured up to the band, never beyond it.
    //
    // Shorter than the vertex capacity is fine: missing entries read as 1.0. Null
    // (the default) leaves the stage byte-for-byte unchanged.
    const std::vector<float>* densityScales = nullptr;
};

enum class IsotropicStatus { Success, Cancelled, InvalidInput };

// `reference` is the surface to project onto (usually built from the input
// mesh before remeshing). Pass the same smoothNormalDegrees used to build it
// in `options` so the smoothing threshold is consistent. The mesh must be
// triangulated.
IsotropicStatus isotropicRemesh(Mesh& mesh, const ReferenceSurface& reference,
                                const IsotropicOptions& options, ProgressSink* progress = nullptr,
                                const CancelToken* cancel = nullptr);

}  // namespace cyber::remesh
