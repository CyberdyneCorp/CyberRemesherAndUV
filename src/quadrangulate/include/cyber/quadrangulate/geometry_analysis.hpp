#pragma once

#include <cstddef>
#include <vector>

#include "cyber/core/bvh.hpp"
#include "cyber/core/mesh.hpp"

// Per-vertex geometric salience fields, computed once per solve and shared by
// every stage that has to decide WHERE something should go
// (openspec/changes/add-zremesher-retopology, Phase C1).
//
// The stages that need this all ask variations of the same question — is this a
// place where putting an extraordinary vertex, or coarsening the mesh, would be
// noticed?  Singularity placement wants to avoid salient regions; the sizing
// field wants to spend quads on them; thin-feature preservation wants to refuse
// to collapse them at all. Computing the answer once, in one place, is what
// stops those three from drifting apart.
//
// Every field is indexed by mesh VertexId and sized to `vertexCapacity()`, so a
// caller indexes with `v.value` directly; dead vertices hold zero. The
// influence fields are normalized to [0, 1] so the optimizer's weights are
// comparable to each other rather than to arbitrary units.
namespace cyber::remesh {

struct GeometryAnalysisOptions {
    // World-space radius over which feature and boundary proximity decay to
    // zero. <= 0 derives one from the mesh's bounding box, which is what makes
    // the influence fields scale-invariant.
    float influenceRadius = 0.0f;

    // Thin-feature probe. A single inward ray misses on curved tubes, so a
    // small fan around -n is cast and the nearest opposing hit wins.
    int thicknessFanSamples = 5;
    float thicknessFanDegrees = 25.0f;

    // A vertex counts as thin when the opposing surface is closer than this
    // multiple of the target edge length — i.e. when fewer than this many quads
    // would span the feature. thinFeatureRisk ramps 0 -> 1 below it.
    float thinFeatureFactor = 4.0f;
};

struct GeometryAnalysis {
    // Max normal deviation across the faces around a vertex, over pi. 0 on a
    // flat region, approaching 1 on a knife edge. A proxy for both principal
    // curvatures at once, which is what salience actually depends on — Gaussian
    // curvature alone reports a cylinder as flat.
    std::vector<float> normalizedCurvature;
    // 1 at a tagged feature edge, decaying to 0 at `influenceRadius` along the
    // surface (not through space, so the far side of a thin plate is not
    // "near" a crease on the front).
    std::vector<float> featureInfluence;
    // Same, from open-surface boundary vertices.
    std::vector<float> boundaryInfluence;
    // Distance to the opposing surface along -n, in world units. Infinity where
    // no opposing surface was found (an open sheet, or a probe that escaped).
    std::vector<float> thickness;
    // 0 where the feature is comfortably thicker than the target edge length,
    // ramping to 1 as it thins toward a single edge chain.
    std::vector<float> thinFeatureRisk;

    bool valid = false;

    // Convenience accessors that tolerate a default-constructed analysis, so a
    // consumer can be written once and run with or without one.
    [[nodiscard]] float curvatureAt(VertexId v) const;
    [[nodiscard]] float featureAt(VertexId v) const;
    [[nodiscard]] float boundaryAt(VertexId v) const;
    [[nodiscard]] float thinRiskAt(VertexId v) const;
};

// Compute the analysis for `mesh`. `targetEdgeLength` is what thinFeatureRisk
// is measured against — the same length the remesh is aiming for — so a feature
// is "thin" relative to the mesh being asked for, not in absolute terms.
//
// `bvh` (optional) is the surface acceleration structure for the thickness
// probe. Null skips the probe: thickness stays infinite and thinFeatureRisk
// zero, and every other field is unaffected.
[[nodiscard]] GeometryAnalysis analyzeGeometry(const Mesh& mesh, float targetEdgeLength,
                                               const Bvh* bvh = nullptr,
                                               const GeometryAnalysisOptions& options = {});

}  // namespace cyber::remesh
