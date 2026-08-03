#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <span>
#include <unordered_map>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/core/reference_surface.hpp"
#include "cyber/core/region_solve.hpp"
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

// Region-solve report (add-weave-regional-solve). Empty on a whole-mesh run.
//
// `interfaceIrregular` is MEASURED AND REPORTED, never enforced: forcing it to
// zero is a coupled degree-constrained matching over the interface ring, which
// no local pass solves (three were built and measured — see the change's
// design.md ADDENDUM 2). A non-empty list is information for the caller, not a
// failure. Refusing on it was measured to reject every fixture.
struct RegionReport {
    std::vector<FaceId> solvedFaces;         // live minus frozen
    std::vector<VertexId> interfaceVertices;  // pinned, with an active incident face
    std::vector<VertexId> interfaceIrregular;  // valence != prescription
    int interiorIndexBudget = 0;   // predicted before the solve
    int indexResidual = 0;         // measured after: 0 means the identity balances
    std::size_t interfaceTriangles = 0;  // non-quads incident to the interface
    [[nodiscard]] bool empty() const { return solvedFaces.empty() && interfaceVertices.empty(); }
};

struct PipelineResult {
    RunStatus status = RunStatus::Error;
    Mesh mesh;
    Statistics stats;
    std::vector<IslandDiagnostic> failedIslands;  // reported, never swallowed (spec)
    std::vector<ParameterIssue> parameterIssues;  // clamp warnings from validation
    std::string error;                            // set when status == Error
    RegionReport region;                          // region solves only
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
// `regionFaces` selects a REGION solve (add-weave-regional-solve): only those
// faces are rewritten, everything else keeps its exact geometry, topology and
// element ids, and the solved patch meets the frozen cage on a prescribed
// boundary. Empty (the default) is the whole-mesh pipeline, byte-for-byte
// unchanged — the region branch is entirely behind `if (!regionFaces.empty())`.
//
// The ids index `input`; the pipeline's working copy preserves them. A region
// solve deliberately skips triangulate / weld / orient / islands / hole-fill /
// pure-quads, so it is NOT equivalent to a whole-mesh solve even when the
// region names every face — that case is refused rather than aliased.
//
// `regionValenceOverrides` maps a VertexId::value to a caller-prescribed total
// valence so an authored pole on the interface is not reported as irregular.
//
// `regionReference` (add-region-external-reference) is an EXTERNAL surface the
// region's interior is projected onto. Without it the region path builds its
// reference from the mesh it is REWRITING, which for a Weave Fill is the cage
// plus a grown seed band — so the solve refines that seed and reprojects onto
// its own approximation, losing any Target detail finer than the band. Measured:
// 0.42 quads mean and 1.26 quads MAX interior deviation on rippled geometry.
// nullptr keeps the previous behaviour byte-for-byte. An explicitly supplied
// reference that cannot be projected onto is REFUSED rather than silently
// replaced by the working mesh, which would make the difference invisible from
// outside. Interface vertices are unaffected either way: they are never smoothed
// (Invariant P), so exact landing does not depend on which surface is used.
//
// `densityScales` is the authored density BRUSH
// (add-weave-density-radial-symmetry): per-vertex multipliers on the target edge
// length, indexed by VertexId::value, MULTIPLIED into the curvature-derived
// scales and clamped to the same band. See IsotropicOptions::densityScales for
// the composition rule and why override was rejected. nullptr is byte-identical.
using QuadrangulatorFactory = std::function<std::unique_ptr<IQuadrangulator>()>;
[[nodiscard]] PipelineResult remesh(
    const Mesh& input, const Parameters& rawParams, ProgressSink* progress = nullptr,
    const CancelToken* cancel = nullptr, const QuadrangulatorFactory& quadrangulator = {},
    const QuadrangulatorFactory& fallbackQuadrangulator = {},
    std::span<const FaceId> regionFaces = {},
    const std::unordered_map<Index, int>* regionValenceOverrides = nullptr,
    const ReferenceSurface* regionReference = nullptr,
    const std::vector<float>* densityScales = nullptr);

// Cleanup policy from the canonical parameters, applied per island result:
// KeepLargest keeps only the biggest connected patch, KeepAll keeps
// everything, MinFaces drops patches smaller than the threshold. Exposed for
// tests.
void applySmallPatchPolicy(Mesh& mesh, SmallPatchPolicy policy, int minFaces);

}  // namespace cyber::remesh
