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

// ---------------------------------------------------------------------------
// Whole-result quality score (Phase G)
// ---------------------------------------------------------------------------
namespace {

// Interior angles of one face, in degrees.
void faceAngles(const Mesh& mesh, FaceId f, std::vector<double>& out) {
    const std::vector<VertexId> vs = mesh.faceVertices(f);
    const std::size_t n = vs.size();
    if (n < 3) {
        return;
    }
    for (std::size_t k = 0; k < n; ++k) {
        const Vec3 p = mesh.position(vs[k]);
        const Vec3 a = mesh.position(vs[(k + n - 1) % n]) - p;
        const Vec3 b = mesh.position(vs[(k + 1) % n]) - p;
        const float la = length(a);
        const float lb = length(b);
        if (la < 1e-20f || lb < 1e-20f) {
            continue;
        }
        const double c = std::clamp(static_cast<double>(dot(a, b) / (la * lb)), -1.0, 1.0);
        out.push_back(std::acos(c) * 180.0 / 3.14159265358979323846);
    }
}

double median(std::vector<double>& v) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

double percentile(std::vector<double>& values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    const double scaled = fraction * static_cast<double>(values.size() - 1);
    const std::size_t index = static_cast<std::size_t>(std::ceil(scaled));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

bool isFinite(Vec3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

void checkFiniteVertices(const Mesh& mesh, QualityScore& score) {
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vertex{v};
        if (mesh.isAlive(vertex) && !isFinite(mesh.position(vertex))) {
            score.geometryValid = false;
        }
    }
}

void collectEdgeStatistics(const Mesh& mesh, QualityScore& score, std::vector<double>& lengths) {
    for (Index e = 0; e < mesh.edgeCapacity(); ++e) {
        const EdgeId edge{e};
        if (!mesh.isAlive(edge)) {
            continue;
        }
        const std::size_t incident = mesh.edgeFaceCount(edge);
        if (incident == 1) {
            ++score.boundaryEdges;
        } else if (incident > 2) {
            ++score.nonManifoldEdges;
        }
        const auto [a, b] = mesh.edgeVertices(edge);
        const double edgeLength = static_cast<double>(length(mesh.position(b) - mesh.position(a)));
        if (!std::isfinite(edgeLength) || edgeLength <= 1e-20) {
            ++score.degenerateEdges;
            score.geometryValid = false;
            continue;
        }
        lengths.push_back(edgeLength);
    }
}

void scoreAngleTail(std::vector<double> angles, QualityScore& score) {
    score.medianAngleDegrees = median(angles);
    std::vector<double> deviations;
    deviations.reserve(angles.size());
    for (const double cornerAngle : angles) {
        if (!std::isfinite(cornerAngle)) {
            score.geometryValid = false;
            continue;
        }
        deviations.push_back(std::abs(cornerAngle - 90.0));
    }
    if (deviations.empty()) {
        score.geometryValid = false;
        return;
    }
    score.p95AngleDeviationDegrees = percentile(deviations, 0.95);
    score.angle = std::clamp(1.0 - score.p95AngleDeviationDegrees / 90.0, 0.0, 1.0);
}

}  // namespace

std::size_t boundaryComponentCount(const Mesh& mesh) {
    std::vector<unsigned char> visited(mesh.vertexCapacity(), 0);
    std::vector<VertexId> stack;
    std::size_t components = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId start{i};
        if (!mesh.isAlive(start) || visited[i] != 0) {
            continue;
        }
        bool touchesBoundary = false;
        for (const EdgeId edge : mesh.vertexEdges(start)) {
            if (mesh.isAlive(edge) && mesh.isBoundaryEdge(edge)) {
                touchesBoundary = true;
                break;
            }
        }
        if (!touchesBoundary) {
            continue;
        }
        ++components;
        visited[i] = 1;
        stack.push_back(start);
        while (!stack.empty()) {
            const VertexId vertex = stack.back();
            stack.pop_back();
            for (const EdgeId edge : mesh.vertexEdges(vertex)) {
                if (!mesh.isAlive(edge) || !mesh.isBoundaryEdge(edge)) {
                    continue;
                }
                const auto [a, b] = mesh.edgeVertices(edge);
                const VertexId next = a == vertex ? b : a;
                if (visited[next.value] == 0) {
                    visited[next.value] = 1;
                    stack.push_back(next);
                }
            }
        }
    }
    return components;
}

QualityScore scoreQuality(const Mesh& mesh, const SingularityMetrics* singularity, bool closedInput,
                          const QualityWeights& weights) {
    QualityScore out;
    std::vector<double> angles;
    checkFiniteVertices(mesh, out);
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        ++out.faces;
        if (mesh.faceSize(face) != 4) {
            ++out.nonQuadFaces;
        }
        if (out.geometryValid) {
            faceAngles(mesh, face, angles);
        }
    }
    if (out.faces == 0) {
        out.geometryValid = false;
        return out;
    }

    // Edge statistics: lengths for uniformity, incidence for defects.
    std::vector<double> lengths;
    collectEdgeStatistics(mesh, out, lengths);
    out.boundaryComponents = boundaryComponentCount(mesh);

    // Angle quality uses the 95th-percentile deviation from 90 degrees so a
    // small tail of visibly distorted corners cannot hide behind the median.
    scoreAngleTail(std::move(angles), out);

    // Edge uniformity as 1 - coefficient of variation, so a perfectly uniform
    // mesh scores 1 and a wildly varying one approaches 0.
    if (!lengths.empty()) {
        double sum = 0.0;
        for (const double l : lengths) {
            sum += l;
        }
        const double mean = sum / static_cast<double>(lengths.size());
        double var = 0.0;
        for (const double l : lengths) {
            var += (l - mean) * (l - mean);
        }
        var /= static_cast<double>(lengths.size());
        out.edgeLengthCv = mean > 1e-12 ? std::sqrt(var) / mean : 0.0;
        out.edgeUniformity = std::clamp(1.0 - out.edgeLengthCv, 0.0, 1.0);
    }

    out.quadPurity = 1.0 - static_cast<double>(out.nonQuadFaces) / static_cast<double>(out.faces);

    // Irregular interior vertices. A quad mesh cannot avoid these entirely, but
    // how many it needs is exactly what separates one cross field from another,
    // so the score has to see them or it cannot rank the candidates.
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        bool interior = true;
        std::size_t valence = 0;
        for (const EdgeId e : mesh.vertexEdges(vid)) {
            if (!mesh.isAlive(e)) {
                continue;
            }
            ++valence;
            if (mesh.edgeFaceCount(e) != 2) {
                interior = false;
            }
        }
        if (!interior || valence == 0) {
            continue;
        }
        ++out.interiorVertices;
        if (valence != 4) {
            ++out.irregularVertices;
        }
    }
    if (out.interiorVertices != 0) {
        out.irregularFraction =
            static_cast<double>(out.irregularVertices) / static_cast<double>(out.interiorVertices);
    }

    // Defects: non-manifold edges always, plus holes when the input was closed.
    const std::size_t defects =
        out.nonManifoldEdges + (closedInput ? out.boundaryEdges : std::size_t{0});
    out.topologicalDefects = static_cast<double>(defects) / static_cast<double>(out.faces);

    // Cone placement, normalized per face so the term does not simply grow with
    // mesh size.
    if (singularity != nullptr && singularity->count != 0) {
        out.singularityCost = singularity->weightedCost / static_cast<double>(out.faces);
    }

    out.total = weights.angle * out.angle + weights.edgeUniformity * out.edgeUniformity +
                weights.quadPurity * out.quadPurity - weights.singularity * out.singularityCost -
                weights.irregular * out.irregularFraction -
                weights.topologicalDefect * out.topologicalDefects;
    return out;
}

bool candidateBeats(const QualityScore& b, const QualityScore& a,
                    CandidateSelectionContext context) {
    const auto isEligible = [context](const QualityScore& score) {
        return score.geometryValid && score.nonManifoldEdges == 0 &&
               score.boundaryComponents == context.expectedBoundaryComponents;
    };
    const bool eligibleA = isEligible(a);
    const bool eligibleB = isEligible(b);
    if (eligibleA != eligibleB) {
        return eligibleB;
    }
    if (!eligibleA) {
        // Every candidate is invalid. Retain a deterministic least-bad choice
        // for diagnostics, without letting aesthetics outrank a topology fault.
        const std::size_t mismatchA =
            a.boundaryComponents > context.expectedBoundaryComponents
                ? a.boundaryComponents - context.expectedBoundaryComponents
                : context.expectedBoundaryComponents - a.boundaryComponents;
        const std::size_t mismatchB =
            b.boundaryComponents > context.expectedBoundaryComponents
                ? b.boundaryComponents - context.expectedBoundaryComponents
                : context.expectedBoundaryComponents - b.boundaryComponents;
        const std::size_t defectsA = a.nonManifoldEdges + mismatchA;
        const std::size_t defectsB = b.nonManifoldEdges + mismatchB;
        if (defectsA != defectsB) {
            return defectsB < defectsA;
        }
    }
    if (b.total != a.total) {
        return b.total > a.total;
    }
    // Everything equal so far: prefer fewer non-quads, then decline — the
    // caller's candidate order is the final, deterministic tie-break.
    return b.nonQuadFaces < a.nonQuadFaces;
}

}  // namespace cyber::remesh
