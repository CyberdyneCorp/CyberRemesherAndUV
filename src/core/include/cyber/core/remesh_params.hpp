#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cyber/core/guidance.hpp"

namespace cyber::remesh {

// Canonical user-facing parameters — the single source of truth shared by
// every entry point (remeshing-parameters spec). Defaults and valid ranges
// live here and nowhere else.
enum class SmallPatchPolicy { KeepLargest, KeepAll, MinFaces };

struct Parameters {
    int targetQuadCount = 50'000;      // 100 .. 2'000'000
    float edgeScale = 1.0f;            // 0.5 .. 4.0
    float sharpEdgeDegrees = 90.0f;    // 30.0 .. 180.0
    float smoothNormalDegrees = 0.0f;  // 0.0 .. 180.0
    float adaptivity = 1.0f;           // 0.0 .. 1.0
    bool pureQuads = false;
    int holeFillMaxBoundary = 64;  // 0 (never fill) .. 10'000
    SmallPatchPolicy smallPatchPolicy = SmallPatchPolicy::KeepLargest;
    int smallPatchMinFaces = 0;  // used when policy == MinFaces
};

struct ParameterIssue {
    std::string parameter;
    std::string message;
    bool fatal = false;  // NaN and other unusable values; clamps are warnings
};

struct ValidatedParameters {
    Parameters params;
    std::vector<ParameterIssue> issues;

    [[nodiscard]] bool ok() const {
        for (const auto& issue : issues) {
            if (issue.fatal) {
                return false;
            }
        }
        return true;
    }
};

// Clamps out-of-range values (warning naming parameter, original and clamped
// value) and flags non-finite values as fatal. Every entry point SHALL call
// this before the pipeline starts (remeshing-parameters spec, "Validation at
// every entry point").
[[nodiscard]] ValidatedParameters validate(const Parameters& raw);

// Validated guidance (remeshing-parameters spec, "Guide and density parameters
// are validated"). Guidance is deliberately NOT part of `Parameters`, which is
// the POD mirror of the C ABI's CyberRemeshParams and must stay POD.
struct ValidatedGuidance {
    Guidance guidance;
    std::vector<ParameterIssue> issues;

    [[nodiscard]] bool ok() const {
        for (const auto& issue : issues) {
            if (issue.fatal) {
                return false;
            }
        }
        return true;
    }
};

// Clamp-and-warn: guide strength outside [kGuideStrengthMin, kGuideStrengthMax]
// and density values outside [kDensityMin, kDensityMax] (one aggregated issue
// naming the clamped count and the effective range, not a per-value flood).
//
// Fatal-and-reject — the "rejected LOUDLY" half of the spec: a guide with fewer
// than two points, a non-finite coordinate / strength / radius, a radius <= 0
// (which could never be honored, so accepting it silently is exactly the
// competitor's quiet-failure bug), a density array carrying both per-vertex and
// per-face values, and a density array whose length matches neither the
// Target's live vertex nor live face count.
[[nodiscard]] ValidatedGuidance validateGuidance(const Guidance& raw, std::size_t targetVertexCount,
                                                 std::size_t targetFaceCount);

// Derived target edge length from total surface area (guarded: non-positive
// area or quad count is an error, never a division by zero — the AutoRemesher
// `--target-quads 0` inf/NaN defect is spec'd away).
struct EdgeLengthResult {
    float edgeLength = 0.0f;
    std::string error;  // empty on success
    [[nodiscard]] bool ok() const { return error.empty(); }
};
[[nodiscard]] EdgeLengthResult targetEdgeLength(double totalArea, int targetQuadCount,
                                                float edgeScale);

}  // namespace cyber::remesh
