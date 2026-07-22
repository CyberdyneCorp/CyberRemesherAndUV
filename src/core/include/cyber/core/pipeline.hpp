#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/core/remesh_params.hpp"

namespace cyber::remesh {

// Automatic remeshing pipeline (remeshing-pipeline spec): target edge length
// -> islands -> per-island adaptive isotropic remesh -> quadrangulation ->
// cleanup policies -> deterministic merge. The input mesh is never modified;
// results commit atomically into the returned value (spec: cancellation
// leaves the document untouched).
enum class RunStatus { Success, Partial, Cancelled, Error };

struct IslandDiagnostic {
    std::size_t islandIndex = 0;
    std::size_t inputFaces = 0;
    std::string stage;   // "isotropic" | "quadrangulate" | ...
    std::string reason;  // human-readable failure cause
};

struct Statistics {
    std::size_t vertexCount = 0;
    std::size_t quadCount = 0;
    std::size_t triangleCount = 0;
    std::size_t otherPolygonCount = 0;
    std::size_t islandCount = 0;
    std::size_t islandsFailed = 0;
    std::size_t holesFilled = 0;
    float targetEdgeLength = 0.0f;
};

struct PipelineResult {
    RunStatus status = RunStatus::Error;
    Mesh mesh;
    Statistics stats;
    std::vector<IslandDiagnostic> failedIslands;  // reported, never swallowed (spec)
    std::vector<ParameterIssue> parameterIssues;  // clamp warnings from validation
    std::string error;                            // set when status == Error
};

// Progress mapping across stages: isotropic 0.0-0.3, quadrangulation
// 0.3-0.9, merge/cleanup 0.9-1.0 (weighted by island face count).
//
// `quadrangulator` selects the quad stage: nullptr uses the built-in greedy
// pairing baseline (keeps golden baselines stable), or pass a factory so the
// caller can inject the field-aligned quadrangulator (which lives in the
// accel-dependent quadrangulate module). The factory is called once per island
// so each island gets an independent instance.
//
// `fallbackQuadrangulator` is used only when the primary is the quad-cover method
// (which skips our isotropic remesh and can decline an island when the seamless-UV
// solve is unavailable or fails). On such a per-island failure the island — left
// untouched by quad-cover — is recovered through the normal isotropic remesh + this
// fallback quadrangulator, so making quad-cover the default never sacrifices the
// always-produces-output robustness of the field-aligned path. Ignored for other
// methods; nullptr disables recovery (the island is simply reported failed).
using QuadrangulatorFactory = std::function<std::unique_ptr<IQuadrangulator>()>;
[[nodiscard]] PipelineResult remesh(const Mesh& input, const Parameters& rawParams,
                                    ProgressSink* progress = nullptr,
                                    const CancelToken* cancel = nullptr,
                                    const QuadrangulatorFactory& quadrangulator = {},
                                    const QuadrangulatorFactory& fallbackQuadrangulator = {});

// Cleanup policy from the canonical parameters, applied per island result:
// KeepLargest keeps only the biggest connected patch, KeepAll keeps
// everything, MinFaces drops patches smaller than the threshold. Exposed for
// tests.
void applySmallPatchPolicy(Mesh& mesh, SmallPatchPolicy policy, int minFaces);

}  // namespace cyber::remesh
