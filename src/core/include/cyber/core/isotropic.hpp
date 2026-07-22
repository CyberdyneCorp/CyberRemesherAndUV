#pragma once

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/reference_surface.hpp"

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
struct IsotropicOptions {
    float targetEdgeLength = 0.0f;  // required, > 0
    int iterations = 3;
    float adaptivity = 0.0f;           // 0 = uniform; curvature-adaptive otherwise
    float smoothNormalDegrees = 0.0f;  // 0 = flat projection; > 0 = PN-triangle smoothing
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
