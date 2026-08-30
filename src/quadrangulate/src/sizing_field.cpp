#include "cyber/quadrangulate/sizing_field.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cyber::remesh {
namespace {

// Curvature shortens edges where the surface bends, so a curved region gets
// more quads for the same total. `curvature` is already normalized to [0, 1].
float curvatureScale(float curvature, float weight) {
    // 1 on a flat region down to 1/(1 + weight) on a knife edge — bounded by
    // construction, so no amount of curvature can drive the target to zero.
    return 1.0f / (1.0f + weight * std::clamp(curvature, 0.0f, 1.0f));
}

// Painted density composes with the area-derived target count as
// localEdgeLength = base / sqrt(density) — the relation the guidance header
// documents, reproduced here so the unified field means the same thing the
// existing density path does.
float densityScale(float density, float weight) {
    const float d = std::max(density, 1e-6f);
    const float scale = 1.0f / std::sqrt(d);
    return 1.0f + weight * (scale - 1.0f);
}

// Near a crease, shorter edges resolve the feature instead of cutting it.
float featureScale(float influence, float weight) {
    return 1.0f / (1.0f + weight * std::clamp(influence, 0.0f, 1.0f));
}

// A thin plate or tube must keep enough quads across it to stay non-degenerate,
// so risk shortens the target hard.
float thinScale(float risk, float weight) {
    return 1.0f / (1.0f + 2.0f * weight * std::clamp(risk, 0.0f, 1.0f));
}

// Neighbourhood smoothing in LOG space. Edge length is multiplicative: an
// arithmetic mean of 0.5 and 2.0 is 1.25, biasing every smoothed neighbourhood
// coarser, while the log-space mean is 1.0.
void smoothLog(const Mesh& mesh, std::vector<float>& value, int passes) {
    if (passes <= 0 || value.empty()) {
        return;
    }
    std::vector<float> logValue(value.size(), 0.0f);
    for (std::size_t i = 0; i < value.size(); ++i) {
        logValue[i] = value[i] > 0.0f ? std::log(value[i]) : 0.0f;
    }
    std::vector<float> next = logValue;
    for (int pass = 0; pass < passes; ++pass) {
        for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
            const VertexId vid{v};
            if (!mesh.isAlive(vid)) {
                continue;
            }
            float acc = logValue[v];
            int count = 1;
            for (const EdgeId e : mesh.vertexEdges(vid)) {
                if (!mesh.isAlive(e)) {
                    continue;
                }
                const auto [a, b] = mesh.edgeVertices(e);
                const VertexId other = a == vid ? b : a;
                if (other.valid() && other.value < logValue.size()) {
                    acc += logValue[other.value];
                    ++count;
                }
            }
            next[v] = acc / static_cast<float>(count);
        }
        logValue.swap(next);
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = std::exp(logValue[i]);
    }
}

}  // namespace

float SizingField::at(VertexId v) const {
    if (!valid || !v.valid() || v.value >= targetEdgeLength.size()) {
        return baseEdgeLength;
    }
    return targetEdgeLength[v.value];
}

float SizingField::at(FaceId f, const Mesh& mesh) const {
    if (!valid || !f.valid() || !mesh.isAlive(f)) {
        return baseEdgeLength;
    }
    float acc = 0.0f;
    int count = 0;
    for (const VertexId v : mesh.faceVertices(f)) {
        acc += at(v);
        ++count;
    }
    return count != 0 ? acc / static_cast<float>(count) : baseEdgeLength;
}

float SizingField::minLength() const {
    if (!valid || targetEdgeLength.empty()) {
        return baseEdgeLength;
    }
    return *std::min_element(targetEdgeLength.begin(), targetEdgeLength.end());
}

float SizingField::maxLength() const {
    if (!valid || targetEdgeLength.empty()) {
        return baseEdgeLength;
    }
    return *std::max_element(targetEdgeLength.begin(), targetEdgeLength.end());
}

SizingField buildSizingField(const Mesh& mesh, const GuidanceField* guidance,
                             const GeometryAnalysis& analysis, const SizingParams& params) {
    SizingField out;
    out.baseEdgeLength = params.baseEdgeLength;
    if (mesh.vertexCapacity() == 0 || params.baseEdgeLength <= 0.0f) {
        return out;
    }
    const float lo = params.baseEdgeLength * std::max(params.minScale, 1e-4f);
    const float hi = params.baseEdgeLength * std::max(params.maxScale, params.minScale);

    out.targetEdgeLength.assign(mesh.vertexCapacity(), params.baseEdgeLength);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        float h = params.baseEdgeLength;
        // Curvature is the one term the pipeline's `adaptivity` parameter
        // gates, so it is interpolated toward 1 rather than weighted away —
        // adaptivity 0 must mean exactly uniform, not "weakly curved".
        const float curved = curvatureScale(analysis.curvatureAt(vid), params.curvatureWeight);
        h *= 1.0f + std::clamp(params.adaptivity, 0.0f, 1.0f) * (curved - 1.0f);
        if (guidance != nullptr) {
            h *= densityScale(guidance->densityAt(mesh.position(vid)), params.densityWeight);
        }
        h *= featureScale(analysis.featureAt(vid), params.featureWeight);
        h *= thinScale(analysis.thinRiskAt(vid), params.thinFeatureWeight);
        out.targetEdgeLength[v] = std::clamp(h, lo, hi);
    }
    smoothLog(mesh, out.targetEdgeLength, params.smoothingPasses);
    // Smoothing can pull a value back out of range at a boundary between
    // strongly different neighbourhoods; the bounds are a contract, so re-clamp.
    for (float& h : out.targetEdgeLength) {
        h = std::clamp(h, lo, hi);
    }
    out.valid = true;
    return out;
}

}  // namespace cyber::remesh
