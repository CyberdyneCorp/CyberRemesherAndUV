#include "cyber/core/pipeline.hpp"

#include <algorithm>
#include <cmath>

#include "cyber/core/bvh.hpp"
#include "cyber/core/isotropic.hpp"
#include "cyber/core/quadrangulate.hpp"
#include "cyber/core/reference_surface.hpp"

namespace cyber::remesh {

namespace {

double totalSurfaceArea(const Mesh& mesh) {
    double area = 0.0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const auto verts = mesh.faceVertices(f);
        for (std::size_t i = 2; i < verts.size(); ++i) {
            const Vec3 a = mesh.position(verts[0]);
            const Vec3 b = mesh.position(verts[i - 1]);
            const Vec3 c = mesh.position(verts[i]);
            area += 0.5 * static_cast<double>(length(cross(b - a, c - a)));
        }
    }
    return area;
}

// Extracts one island into a standalone mesh (local vertex indexing).
Mesh extractIsland(const Mesh& source, const std::vector<FaceId>& faces) {
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faceLists;
    std::vector<Index> remap(source.vertexCapacity(), kInvalidIndex);
    for (const FaceId f : faces) {
        std::vector<Index> face;
        for (const VertexId v : source.faceVertices(f)) {
            if (remap[v.value] == kInvalidIndex) {
                remap[v.value] = static_cast<Index>(positions.size());
                positions.push_back(source.position(v));
            }
            face.push_back(remap[v.value]);
        }
        faceLists.push_back(std::move(face));
    }
    return Mesh::fromIndexed(positions, faceLists);
}

void countFaces(const Mesh& mesh, Statistics& stats) {
    stats.vertexCount = mesh.vertexCount();
    stats.quadCount = 0;
    stats.triangleCount = 0;
    stats.otherPolygonCount = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        const std::size_t n = mesh.faceSize(FaceId{fi});
        if (n == 4) {
            ++stats.quadCount;
        } else if (n == 3) {
            ++stats.triangleCount;
        } else {
            ++stats.otherPolygonCount;
        }
    }
}

}  // namespace

void applySmallPatchPolicy(Mesh& mesh, SmallPatchPolicy policy, int minFaces) {
    if (policy == SmallPatchPolicy::KeepAll) {
        return;
    }
    const auto patches = mesh.islands();
    if (patches.size() <= 1) {
        return;
    }
    std::size_t largest = 0;
    for (std::size_t i = 1; i < patches.size(); ++i) {
        if (patches[i].size() > patches[largest].size()) {
            largest = i;
        }
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const bool keep = policy == SmallPatchPolicy::KeepLargest
                              ? i == largest
                              : patches[i].size() >= static_cast<std::size_t>(minFaces);
        if (keep) {
            continue;
        }
        for (const FaceId f : patches[i]) {
            mesh.removeFace(f);
        }
    }
}

PipelineResult remesh(const Mesh& input, const Parameters& rawParams, ProgressSink* progress,
                      const CancelToken* cancel, const QuadrangulatorFactory& quadrangulator) {
    PipelineResult result;

    // Stage 0: parameters (validated at every entry point — spec).
    ValidatedParameters validated = validate(rawParams);
    result.parameterIssues = validated.issues;
    if (!validated.ok()) {
        result.status = RunStatus::Error;
        result.error = "invalid parameters";
        return result;
    }
    const Parameters& params = validated.params;

    if (input.faceCount() == 0) {
        result.status = RunStatus::Error;
        result.error = "input mesh is empty";
        return result;
    }

    // Pure quads: remesh at quarter density, then one linear subdivision
    // turns every face into quads at the requested density (spec: pure-quad
    // option with honest residual reporting — this construction leaves zero
    // residual non-quads only after subdividing, so triangles at quarter
    // density become 3 quads).
    const int effectiveQuads =
        params.pureQuads ? std::max(25, params.targetQuadCount / 4) : params.targetQuadCount;

    // Stage 1: guarded target edge length.
    Mesh work = input;
    work.triangulate();
    const double area = totalSurfaceArea(work);
    const EdgeLengthResult lengthResult = targetEdgeLength(area, effectiveQuads, params.edgeScale);
    if (!lengthResult.ok()) {
        result.status = RunStatus::Error;
        result.error = lengthResult.error;
        return result;
    }
    result.stats.targetEdgeLength = lengthResult.edgeLength;

    // Stage 2: islands (face-complete, deterministic order — mesh-core spec).
    const auto islandFaces = work.islands();
    result.stats.islandCount = islandFaces.size();

    struct IslandOutcome {
        Mesh mesh;
        bool ok = false;
        std::string stage;
        std::string reason;
        std::size_t inputFaces = 0;
    };
    std::vector<IslandOutcome> outcomes(islandFaces.size());

    // Weighted progress: islands contribute proportionally to face count.
    std::size_t totalFaces = 0;
    for (const auto& faces : islandFaces) {
        totalFaces += faces.size();
    }

    float progressBase = 0.0f;
    for (std::size_t i = 0; i < islandFaces.size(); ++i) {
        IslandOutcome& outcome = outcomes[i];
        outcome.inputFaces = islandFaces[i].size();
        const float weight = totalFaces > 0 ? static_cast<float>(islandFaces[i].size()) /
                                                  static_cast<float>(totalFaces)
                                            : 0.0f;

        if (cancel && cancel->isCancelled()) {
            result.status = RunStatus::Cancelled;
            return result;
        }

        outcome.mesh = extractIsland(work, islandFaces[i]);
        outcome.mesh.tagFeatureEdges(params.sharpEdgeDegrees);
        const ReferenceSurface reference(outcome.mesh, params.smoothNormalDegrees);

        // Isotropic stage: overall 0.0-0.3 of this island's slice.
        IsotropicOptions iso;
        iso.targetEdgeLength = lengthResult.edgeLength;
        iso.adaptivity = params.adaptivity;
        iso.smoothNormalDegrees = params.smoothNormalDegrees;
        ProgressSink isoSink =
            progress ? progress->subrange(progressBase, progressBase + weight * 0.3f, "isotropic")
                     : ProgressSink{};
        const IsotropicStatus isoStatus =
            isotropicRemesh(outcome.mesh, reference, iso, progress ? &isoSink : nullptr, cancel);
        if (isoStatus == IsotropicStatus::Cancelled) {
            result.status = RunStatus::Cancelled;
            return result;
        }
        if (isoStatus != IsotropicStatus::Success || outcome.mesh.faceCount() == 0) {
            outcome.stage = "isotropic";
            outcome.reason = isoStatus == IsotropicStatus::InvalidInput
                                 ? "invalid island input"
                                 : "island vanished during isotropic remeshing";
            progressBase += weight;
            continue;
        }

        // Quadrangulation stage: 0.3-0.9 of this island's slice. Use the
        // injected quadrangulator (field-aligned) when provided, else greedy.
        std::unique_ptr<IQuadrangulator> quad =
            quadrangulator ? quadrangulator() : makeGreedyPairingQuadrangulator();
        ProgressSink quadSink =
            progress ? progress->subrange(progressBase + weight * 0.3f,
                                          progressBase + weight * 0.9f, "quadrangulate")
                     : ProgressSink{};
        const auto quadOutcome = quad->quadrangulate(
            outcome.mesh, lengthResult.edgeLength, progress ? &quadSink : nullptr, cancel);
        if (quadOutcome.cancelled) {
            result.status = RunStatus::Cancelled;
            return result;
        }
        if (!quadOutcome.success || outcome.mesh.faceCount() == 0) {
            outcome.stage = "quadrangulate";
            outcome.reason =
                quadOutcome.failureReason.empty() ? "no faces produced" : quadOutcome.failureReason;
            progressBase += weight;
            continue;
        }

        applySmallPatchPolicy(outcome.mesh, params.smallPatchPolicy, params.smallPatchMinFaces);
        outcome.ok = outcome.mesh.faceCount() > 0;
        if (!outcome.ok) {
            outcome.stage = "cleanup";
            outcome.reason = "patch policy removed all faces";
        }
        progressBase += weight;
    }

    // Stage 3: deterministic merge (island order), 0.9-1.0.
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        const IslandOutcome& outcome = outcomes[i];
        if (!outcome.ok) {
            ++result.stats.islandsFailed;
            result.failedIslands.push_back({i, outcome.inputFaces, outcome.stage, outcome.reason});
            continue;
        }
        std::vector<Vec3> islandPositions;
        std::vector<std::vector<Index>> islandFaceLists;
        outcome.mesh.toIndexed(islandPositions, islandFaceLists);
        const Index offset = static_cast<Index>(positions.size());
        positions.insert(positions.end(), islandPositions.begin(), islandPositions.end());
        for (auto& face : islandFaceLists) {
            for (Index& v : face) {
                v += offset;
            }
            faces.push_back(std::move(face));
        }
    }
    if (progress) {
        progress->report(0.95f, "merge");
    }

    result.mesh = Mesh::fromIndexed(positions, faces);
    // Cleanup: fill holes up to the boundary-length limit before the optional
    // pure-quad subdivision, so filled patches subdivide into quads too
    // (remeshing-parameters spec, "holeFillMaxBoundary"; 0/<3 disables).
    if (params.holeFillMaxBoundary >= 3 && result.mesh.faceCount() > 0) {
        result.stats.holesFilled =
            result.mesh.fillHoles(static_cast<std::size_t>(params.holeFillMaxBoundary));
    }
    if (params.pureQuads && result.mesh.faceCount() > 0) {
        result.mesh = result.mesh.linearSubdivide();
        // Linear subdivision only splits faces — the new vertices sit on the
        // coarse (quarter-density) base's flat facets, so the silhouette stays
        // faceted/jagged. Snap every vertex back onto the source surface so the
        // pure-quad result follows the original curvature.
        const Bvh sourceSurface(work);
        if (!sourceSurface.empty()) {
            for (Index vi = 0; vi < result.mesh.vertexCapacity(); ++vi) {
                const VertexId v{vi};
                if (result.mesh.isAlive(v)) {
                    result.mesh.setPosition(v, sourceSurface.closestPoint(result.mesh.position(v)).point);
                }
            }
        }
    }
    countFaces(result.mesh, result.stats);

    if (result.mesh.faceCount() == 0) {
        result.status = RunStatus::Error;
        result.error = "remeshing produced no result";
        return result;
    }
    result.status = result.failedIslands.empty() ? RunStatus::Success : RunStatus::Partial;
    if (progress) {
        progress->report(1.0f, "done");
    }
    return result;
}

}  // namespace cyber::remesh
