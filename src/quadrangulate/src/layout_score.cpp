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
    salience += weights.featureProximity * static_cast<double>(geometry.featureAt(v));
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
        out.worstCost = std::max(out.worstCost, cost);
        const double feature = static_cast<double>(geometry.featureAt(n.vertex));
        featureSum += feature;
        out.featureInfluenceMax = std::max(out.featureInfluenceMax, feature);
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
