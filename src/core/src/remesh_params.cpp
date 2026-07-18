#include "cyber/core/remesh_params.hpp"

#include <algorithm>
#include <cmath>

namespace cyber::remesh {

namespace {

void clampInt(int& value, int lo, int hi, const char* name, std::vector<ParameterIssue>& issues) {
    if (value < lo || value > hi) {
        const int clamped = std::clamp(value, lo, hi);
        issues.push_back({name,
                          std::string(name) + " " + std::to_string(value) + " out of range [" +
                              std::to_string(lo) + ", " + std::to_string(hi) + "], clamped to " +
                              std::to_string(clamped),
                          false});
        value = clamped;
    }
}

void clampFloat(float& value, float lo, float hi, const char* name,
                std::vector<ParameterIssue>& issues) {
    if (!std::isfinite(value)) {
        issues.push_back({name, std::string(name) + " is not a finite number", true});
        value = lo;
        return;
    }
    if (value < lo || value > hi) {
        const float clamped = std::clamp(value, lo, hi);
        issues.push_back({name,
                          std::string(name) + " " + std::to_string(value) + " out of range [" +
                              std::to_string(lo) + ", " + std::to_string(hi) + "], clamped to " +
                              std::to_string(clamped),
                          false});
        value = clamped;
    }
}

}  // namespace

ValidatedParameters validate(const Parameters& raw) {
    ValidatedParameters out;
    out.params = raw;
    Parameters& p = out.params;

    clampInt(p.targetQuadCount, 100, 2'000'000, "targetQuadCount", out.issues);
    clampFloat(p.edgeScale, 0.5f, 4.0f, "edgeScale", out.issues);
    clampFloat(p.sharpEdgeDegrees, 30.0f, 180.0f, "sharpEdgeDegrees", out.issues);
    clampFloat(p.smoothNormalDegrees, 0.0f, 180.0f, "smoothNormalDegrees", out.issues);
    clampFloat(p.adaptivity, 0.0f, 1.0f, "adaptivity", out.issues);
    clampInt(p.holeFillMaxBoundary, 0, 10'000, "holeFillMaxBoundary", out.issues);
    if (p.smallPatchPolicy == SmallPatchPolicy::MinFaces) {
        clampInt(p.smallPatchMinFaces, 0, 1'000'000, "smallPatchMinFaces", out.issues);
    }
    return out;
}

EdgeLengthResult targetEdgeLength(double totalArea, int targetQuadCount, float edgeScale) {
    if (targetQuadCount <= 0) {
        return {0.0f, "target quad count must be positive"};
    }
    if (!(totalArea > 0.0) || !std::isfinite(totalArea)) {
        return {0.0f, "mesh surface area must be positive"};
    }
    // Two triangles per quad; invert the equilateral-triangle area formula
    // (same derivation as AutoRemesher, with the zero guard it lacked).
    const double triangleCount = 2.0 * targetQuadCount;
    const double perTriangle = totalArea / triangleCount;
    constexpr double kEquilateralFactor = 0.86602540378 * 0.5;
    const double length =
        std::sqrt(perTriangle / kEquilateralFactor) * static_cast<double>(edgeScale);
    if (!std::isfinite(length) || length <= 0.0) {
        return {0.0f, "derived edge length is not usable"};
    }
    return {static_cast<float>(length), ""};
}

}  // namespace cyber::remesh
