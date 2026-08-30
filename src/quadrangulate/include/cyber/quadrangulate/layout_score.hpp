#pragma once

#include <cstddef>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/geometry_analysis.hpp"
#include "cyber/quadrangulate/topology_layout.hpp"

// Scoring a topology layout (openspec/changes/add-zremesher-retopology, Phase
// C2).
//
// Quad meshes REQUIRE extraordinary vertices — a closed surface cannot be all
// valence-4 — so counting them is the wrong primary metric. Two layouts with
// the same count can be worlds apart: one puts its cones in broad smooth
// regions where nobody looks, the other puts them on an eye corner, a
// silhouette and a finger bend. What separates them is WHERE the cones are, and
// that is what this scores.
//
// The cost is a weighted sum of per-cone salience terms, each scaled by |index|
// so a valence-6 cone costs twice a valence-5 one. Every term comes from
// `GeometryAnalysis` and is already normalized to [0, 1], so the weights are
// comparable to each other rather than to arbitrary units.
namespace cyber::remesh {

struct SingularityWeights {
    // A flat charge per cone, so removing one is always worth something even
    // where it sits somewhere harmless.
    double count = 1.0;
    // Curved regions read as shape; a cone there distorts what the eye checks.
    double curvature = 2.0;
    // A cone on a crease fights the feature it is supposed to follow.
    double featureProximity = 4.0;
    // A cone across a thin plate or tube can collapse the feature outright.
    double thinFeature = 5.0;
    // A cone on an open border has fewer directions to resolve into.
    double boundary = 3.0;
};

struct SingularityMetrics {
    std::size_t count = 0;
    // Sum of the per-cone costs. THE headline number for Phase C.
    double weightedCost = 0.0;
    // Mean and worst per-cone cost, so a single badly placed cone is visible
    // behind a good average.
    double meanCost = 0.0;
    double worstCost = 0.0;
    // How close cones sit to features, as influence in [0, 1] (1 = on a
    // feature). Reported as mean and max because the max is the one that
    // produces a visible artifact.
    double featureInfluenceMean = 0.0;
    double featureInfluenceMax = 0.0;
    // Total thin-feature exposure across the cones.
    double thinFeaturePenalty = 0.0;
    // Sum of cross-field indices; invariant under any legal relocation.
    int totalIndex = 0;

    // How the cost is distributed, which is what says whether relocation has
    // anything to work with. `onFeature` counts cones sitting exactly on a
    // feature edge — the design's own example of a badly placed cone — and
    // `onFeatureCost` is their share of the total. A large share means moving
    // them is worth machinery; a small one means the ceiling is low however
    // good the optimizer is.
    std::size_t onFeature = 0;
    double onFeatureCost = 0.0;

    // The cost split by term. `countCost` is the irreducible part — every cone
    // charges it wherever it sits — so it bounds what ANY relocation can win:
    // an optimizer can only ever move the difference between the total and it.
    double countCost = 0.0;
    double curvatureCost = 0.0;
    double featureCost = 0.0;
    double thinCost = 0.0;
    double boundaryCost = 0.0;
};

// Score every singularity node of `layout` against `geometry`. Nodes whose
// source vertex is unknown (a layout traced without geometry capture) fall back
// to the flat count charge, so the metric degrades rather than lying.
[[nodiscard]] SingularityMetrics scoreSingularities(const TopologyLayout& layout,
                                                    const GeometryAnalysis& geometry,
                                                    const SingularityWeights& weights = {});

// The cost of ONE cone of the given index sitting at `v`. Exposed because the
// relocation pass (Phase C3) evaluates candidate positions with it, and a
// relocation that used a different cost than the metric would be optimizing
// something nobody measures.
[[nodiscard]] double singularityCost(VertexId v, int index, const GeometryAnalysis& geometry,
                                     const SingularityWeights& weights = {});

// ---------------------------------------------------------------------------
// Whole-result quality score (Phase G)
// ---------------------------------------------------------------------------
//
// One number per candidate solve, so two solves can be COMPARED rather than
// argued about. This is what the repo's own open question needs — the roadmap
// records that no static "organic vs CAD" threshold can pick the better field
// solver for every model, and that choosing per input by MEASURING both is the
// real answer. A comparable score is the missing half of that.
//
// Every term is normalized to roughly [0, 1] before weighting, so the weights
// mean what they look like. Topological defects are deliberately not a term
// among equals: a mesh with excellent angles and a crack is not a winner, so
// they carry a weight that dominates everything aesthetic.
struct QualityWeights {
    double angle = 1.0;           // quad corners near 90 degrees
    double edgeUniformity = 0.8;  // consistent edge lengths
    double quadPurity = 1.0;      // faces that are actually quads
    double singularity = 1.5;     // penalty, subtracted
    // Irregular (valence != 4) interior vertices, as a fraction. This is the
    // axis the two cross-field candidates actually differ on, so a score
    // without it cannot tell them apart — and the first version of this score
    // omitted it and duly picked the same candidate for every model.
    double irregular = 2.0;            // penalty, subtracted
    double topologicalDefect = 100.0;  // penalty, subtracted; dominates by design
};

struct QualityScore {
    double total = 0.0;

    // Positive terms, each in [0, 1].
    double angle = 0.0;
    double edgeUniformity = 0.0;
    double quadPurity = 0.0;
    // Penalties, already normalized.
    double singularityCost = 0.0;
    double irregularFraction = 0.0;
    double topologicalDefects = 0.0;

    // Raw counts behind the terms, for reporting.
    std::size_t faces = 0;
    std::size_t nonQuadFaces = 0;
    std::size_t boundaryEdges = 0;
    std::size_t nonManifoldEdges = 0;
    double medianAngleDegrees = 0.0;
    double edgeLengthCv = 0.0;
    std::size_t irregularVertices = 0;
    std::size_t interiorVertices = 0;
};

// Score a finished mesh. `singularity` (optional) folds in the layout's cone
// placement cost; without it the topology terms still work off the mesh alone.
// `closedInput` says whether a boundary edge is a DEFECT (a closed input that
// came back with holes) or expected.
[[nodiscard]] QualityScore scoreQuality(const Mesh& mesh, const SingularityMetrics* singularity,
                                        bool closedInput, const QualityWeights& weights = {});

// Pick the better of two scored candidates, deterministically. Defects dominate;
// ties fall through to the total, then the cone count, then candidate order — so
// the same inputs always select the same candidate.
//
// Returns true when `b` should replace `a`.
[[nodiscard]] bool candidateBeats(const QualityScore& b, const QualityScore& a);

}  // namespace cyber::remesh
