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

namespace {

// Per-guide fatal checks. Returns false (and pushes the naming issue) as soon
// as the guide cannot be honored at all.
bool validateOneGuide(FlowGuide& guide, std::size_t index, std::vector<ParameterIssue>& issues) {
    const std::string name = "guides[" + std::to_string(index) + "]";
    if (guide.points.size() < 2) {
        issues.push_back({name,
                          name + " has " + std::to_string(guide.points.size()) +
                              " point(s); a flow guide needs at least 2",
                          true});
        return false;
    }
    for (std::size_t i = 0; i < guide.points.size(); ++i) {
        const Vec3 p = guide.points[i];
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
            continue;
        }
        issues.push_back(
            {name, name + ".points[" + std::to_string(i) + "] is not a finite point", true});
        return false;
    }
    if (!std::isfinite(guide.radius) || guide.radius <= 0.0f) {
        issues.push_back({name,
                          name + ".radius " + std::to_string(guide.radius) +
                              " must be a finite value greater than 0 (a zero-radius guide can "
                              "never be honored)",
                          true});
        return false;
    }
    clampFloat(guide.strength, kGuideStrengthMin, kGuideStrengthMax, (name + ".strength").c_str(),
               issues);
    return true;
}

// Clamp a density array in place; returns the number of values clamped.
std::size_t clampDensityValues(std::vector<float>& values, bool& sawNonFinite) {
    std::size_t clamped = 0;
    for (float& v : values) {
        if (!std::isfinite(v)) {
            sawNonFinite = true;
            return clamped;
        }
        if (v < kDensityMin || v > kDensityMax) {
            v = std::clamp(v, kDensityMin, kDensityMax);
            ++clamped;
        }
    }
    return clamped;
}

}  // namespace

ValidatedGuidance validateGuidance(const Guidance& raw, std::size_t targetVertexCount,
                                   std::size_t targetFaceCount) {
    ValidatedGuidance out;
    out.guidance = raw;

    std::vector<FlowGuide> kept;
    kept.reserve(out.guidance.guides.size());
    for (std::size_t i = 0; i < out.guidance.guides.size(); ++i) {
        FlowGuide guide = out.guidance.guides[i];
        if (validateOneGuide(guide, i, out.issues)) {
            kept.push_back(std::move(guide));
        }
    }
    out.guidance.guides = std::move(kept);

    DensityField& density = out.guidance.density;
    if (!density.vertexValues.empty() && !density.faceValues.empty()) {
        out.issues.push_back({"density",
                              "density carries both per-vertex and per-face values; supply exactly "
                              "one",
                              true});
        return out;
    }
    const bool perVertex = !density.vertexValues.empty();
    std::vector<float>& values = perVertex ? density.vertexValues : density.faceValues;
    if (values.empty()) {
        return out;
    }
    const std::size_t expected = perVertex ? targetVertexCount : targetFaceCount;
    if (values.size() != expected) {
        out.issues.push_back({"density",
                              std::string("density has ") + std::to_string(values.size()) +
                                  (perVertex ? " per-vertex" : " per-face") +
                                  " values but the "
                                  "target has " +
                                  std::to_string(expected),
                              true});
        return out;
    }
    bool sawNonFinite = false;
    const std::size_t clamped = clampDensityValues(values, sawNonFinite);
    if (sawNonFinite) {
        out.issues.push_back(
            {"density", "density contains a value that is not a finite number", true});
        return out;
    }
    if (clamped > 0) {
        out.issues.push_back(
            {"density",
             "density: " + std::to_string(clamped) + " value(s) out of range, clamped to [" +
                 std::to_string(kDensityMin) + ", " + std::to_string(kDensityMax) + "]",
             false});
    }
    // A post-clamp density of 1.0 everywhere is the identity of the sizing
    // relation, so it must leave the run exactly as it was. Dropping it HERE —
    // at the single validation gate every entry point shares — is what makes
    // that true: downstream, a merely-present guidance field also decides which
    // seamless-UV backend runs, so carrying a neutral paint any further would
    // silently change the mesh (remeshing-pipeline spec: byte-identical).
    if (density.isNeutral()) {
        values.clear();
        out.issues.push_back(
            {"density", "density is 1.0 everywhere (no sizing change); ignored", false});
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
