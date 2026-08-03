#pragma once

#include <string>
#include <vector>

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
// `regionSolve` marks a REGION solve (add-weave-regional-solve): a subset of
// faces rewritten in place against frozen surrounding topology. It makes
// `pureQuads` a FATAL issue, because the pure-quad construction rewrites
// geometry wholesale and ends in an unguarded whole-vertex reprojection that
// would move the prescribed boundary. Validating it here rather than only in
// the pipeline stops a direct `cyber::remesh::remesh` caller too.
//
// Deliberately NOT fatal in region mode: holeFillMaxBoundary and
// smallPatchPolicy. Both are DEFAULTS (64 and KeepLargest) and both are
// meaningless for one connected region bounded by frozen topology, so refusing
// would reject every ordinary call. The pipeline skips them and reports a
// non-fatal issue where they are ignored.
[[nodiscard]] ValidatedParameters validate(const Parameters& raw, bool regionSolve = false);

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
