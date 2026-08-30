#pragma once

#include <cstddef>
#include <vector>

#include "cyber/core/guidance.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/geometry_analysis.hpp"

// One explicit target-edge-length field, shared by every stage that decides how
// big a quad should be (openspec/changes/add-zremesher-retopology, Phase D).
//
// Today sizing is INDIRECT and split: painted density is baked into the
// preprocessed mesh's edge lengths, curvature adaptivity is a separate knob on
// the isotropic stage, feature proximity is implicit in the crease tagging, and
// thin features are not represented at all. Each consumer then reconstructs
// what it needs. That works, and it means no single place answers "how big
// should a quad be here?" — so the answers drift apart, and a new input has to
// be threaded into every consumer separately.
//
// `SizingField` is that single place: one value per vertex, in world units,
// that every stage reads.
namespace cyber::remesh {

struct SizingParams {
    // The uniform target edge length the field modulates. Everything below is a
    // multiplier on it.
    float baseEdgeLength = 1.0f;

    // How much of the curvature term to apply, 0 = uniform. Matches the
    // pipeline's existing adaptivity parameter in meaning and range.
    float adaptivity = 0.0f;

    // Per-term weights. 0 removes a term entirely, which is what makes each one
    // separately testable and separately reversible.
    float curvatureWeight = 1.0f;
    float densityWeight = 1.0f;
    float featureWeight = 1.0f;
    float thinFeatureWeight = 1.0f;

    // Bounds on the multiplier, so no single term can run away. A field that
    // can reach zero produces an unbounded quad count.
    float minScale = 0.25f;
    float maxScale = 4.0f;

    // Neighbourhood smoothing passes, applied in LOG space — target edge length
    // is multiplicative, so smoothing it linearly would treat halving and
    // doubling asymmetrically and bias the field coarse.
    int smoothingPasses = 4;
};

class SizingField {
public:
    // Target edge length per mesh VertexId, world units. Empty when the field
    // was never built, in which case `at` falls back to the base length.
    std::vector<float> targetEdgeLength;
    float baseEdgeLength = 1.0f;
    bool valid = false;

    [[nodiscard]] float at(VertexId v) const;
    // Mean of the face's corners — the value a face-indexed consumer wants.
    [[nodiscard]] float at(FaceId f, const Mesh& mesh) const;

    // Smallest and largest target in the field; equal when it is uniform.
    [[nodiscard]] float minLength() const;
    [[nodiscard]] float maxLength() const;
};

// Build the field. `guidance` (optional) supplies painted density; `analysis`
// supplies curvature, feature proximity and thin-feature risk.
//
// With every weight at zero — or with `adaptivity` 0 and no density painted —
// the result is exactly uniform at `baseEdgeLength`, which is what lets the
// field be introduced without changing any existing output.
[[nodiscard]] SizingField buildSizingField(const Mesh& mesh, const GuidanceField* guidance,
                                           const GeometryAnalysis& analysis,
                                           const SizingParams& params);

}  // namespace cyber::remesh
