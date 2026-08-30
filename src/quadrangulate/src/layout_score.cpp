#include "cyber/quadrangulate/layout_score.hpp"

#include <algorithm>
#include <cmath>

namespace cyber::remesh {

double singularityCost(VertexId v, int index, const GeometryAnalysis& geometry,
                       const SingularityWeights& weights) {
    const double magnitude = std::abs(static_cast<double>(index));
    // The salience terms scale with |index|; the flat count charge does not
    // depend on where the cone sits, so it is not multiplied by salience but is
    // still proportional to how extraordinary the cone is.
    double salience = 0.0;
    salience += weights.curvature * static_cast<double>(geometry.curvatureAt(v));
    // A cone AT a feature junction is not a defect — the surface branches there
    // and the quads must too, which is why a cube's eight corner cones are its
    // optimal topology rather than its worst. Only a cone sitting on a crease
    // CURVE, interrupting a loop that should run along it, is charged.
    const double onCurve = static_cast<double>(geometry.featureAt(v)) *
                           (1.0 - static_cast<double>(geometry.cornerAt(v)));
    salience += weights.featureProximity * onCurve;
    salience += weights.thinFeature * static_cast<double>(geometry.thinRiskAt(v));
    salience += weights.boundary * static_cast<double>(geometry.boundaryAt(v));
    return magnitude * (weights.count + salience);
}

SingularityMetrics scoreSingularities(const TopologyLayout& layout,
                                      const GeometryAnalysis& geometry,
                                      const SingularityWeights& weights) {
    SingularityMetrics out;
    double featureSum = 0.0;
    for (const LayoutNode& n : layout.nodes) {
        if (n.kind != LayoutNodeKind::Singularity) {
            continue;
        }
        ++out.count;
        out.totalIndex += n.singularityIndex;
        const double cost = singularityCost(n.vertex, n.singularityIndex, geometry, weights);
        out.weightedCost += cost;
        {
            const double m = std::abs(static_cast<double>(n.singularityIndex));
            const double onCurve = static_cast<double>(geometry.featureAt(n.vertex)) *
                                   (1.0 - static_cast<double>(geometry.cornerAt(n.vertex)));
            out.countCost += m * weights.count;
            out.curvatureCost +=
                m * weights.curvature * static_cast<double>(geometry.curvatureAt(n.vertex));
            out.featureCost += m * weights.featureProximity * onCurve;
            out.thinCost +=
                m * weights.thinFeature * static_cast<double>(geometry.thinRiskAt(n.vertex));
            out.boundaryCost +=
                m * weights.boundary * static_cast<double>(geometry.boundaryAt(n.vertex));
        }
        out.worstCost = std::max(out.worstCost, cost);
        const double feature = static_cast<double>(geometry.featureAt(n.vertex)) *
                               (1.0 - static_cast<double>(geometry.cornerAt(n.vertex)));
        featureSum += feature;
        out.featureInfluenceMax = std::max(out.featureInfluenceMax, feature);
        // "On a feature" means the influence field saturated, i.e. the cone's
        // own vertex is an endpoint of a tagged feature edge.
        if (feature >= 1.0 - 1e-6) {
            ++out.onFeature;
            out.onFeatureCost += cost;
        }
        out.thinFeaturePenalty += static_cast<double>(geometry.thinRiskAt(n.vertex));
    }
    if (out.count != 0) {
        const double n = static_cast<double>(out.count);
        out.meanCost = out.weightedCost / n;
        out.featureInfluenceMean = featureSum / n;
    }
    return out;
}

}  // namespace cyber::remesh
