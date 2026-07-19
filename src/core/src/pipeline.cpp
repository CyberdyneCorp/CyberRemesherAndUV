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

// Tangential Laplacian step for an interior vertex: move toward the 1-ring
// centroid, keeping only the component in the vertex's tangent plane so the
// vertex slides across the surface without denting it inward.
Vec3 tangentialTarget(const Mesh& mesh, VertexId v, float lambda) {
    const auto edges = mesh.vertexEdges(v);
    Vec3 centroid{};
    for (const EdgeId e : edges) {
        const auto [a, b] = mesh.edgeVertices(e);
        centroid += mesh.position(a == v ? b : a);
    }
    centroid = centroid / static_cast<float>(edges.size());
    Vec3 normal{};
    for (const FaceId f : mesh.vertexFaces(v)) {
        normal += mesh.faceNormal(f);
    }
    normal = normalized(normal);
    const Vec3 p = mesh.position(v);
    Vec3 delta = (centroid - p) * lambda;
    delta = delta - normal * dot(delta, normal);  // keep only the tangential slide
    return p + delta;
}

// Relaxes a quad mesh, re-projecting onto the source surface every iteration.
// Linear subdivision followed by closest-point snapping leaves many degenerate
// quads — adjacent vertices land nearly on top of one another (near-zero edges,
// ~0-degree corners), so a pure-quad result is clean topologically but riddled
// with slivers. Interior vertices slide toward their 1-ring centroid to
// equalize quad shape while the per-iteration projection keeps them on the
// source surface, so the silhouette is preserved while element quality
// improves. Feature (sharp crease) and boundary vertices are frozen so creases
// and open borders keep their subdivided positions — sliding them along the
// faceted crease instead reprojects erratically and creates new slivers.
void relaxQuadMesh(Mesh& mesh, const ReferenceSurface& reference, float sharpEdgeDegrees,
                   int iterations, float lambda) {
    mesh.tagFeatureEdges(sharpEdgeDegrees);
    std::vector<bool> constrained(mesh.vertexCapacity(), false);
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || !(mesh.isFeatureEdge(e) || mesh.isBoundaryEdge(e))) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        constrained[a.value] = true;
        constrained[b.value] = true;
    }

    std::vector<Vec3> newPos(mesh.vertexCapacity());
    std::vector<bool> move(mesh.vertexCapacity(), false);
    for (int it = 0; it < iterations; ++it) {
        std::fill(move.begin(), move.end(), false);
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            const VertexId v{i};
            if (!mesh.isAlive(v) || constrained[i] || mesh.vertexEdges(v).empty()) {
                continue;
            }
            newPos[i] = tangentialTarget(mesh, v, lambda);
            move[i] = true;
        }
        for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
            if (!move[i]) {
                continue;
            }
            mesh.setPosition(VertexId{i},
                             reference.empty() ? newPos[i] : reference.project(newPos[i]));
        }
    }
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
        // Relax the coarse base onto the source first: a skewed base subdivides
        // into skewed quads, so smoothing it before the split reduces the sliver
        // tail the subdivision would otherwise inherit.
        {
            const ReferenceSurface baseSurface(work, params.smoothNormalDegrees);
            if (!baseSurface.empty()) {
                relaxQuadMesh(result.mesh, baseSurface, params.sharpEdgeDegrees,
                              /*iterations=*/10, /*lambda=*/0.5f);
            }
        }
        result.mesh = result.mesh.linearSubdivide();
        // Linear subdivision only splits faces — the new vertices sit on the
        // coarse (quarter-density) base's flat facets, so the silhouette stays
        // faceted/jagged AND the split leaves many degenerate slivers. Build a
        // reference surface over the (triangulated) source, seed every vertex
        // onto it, then run tangential relaxation to de-sliver the quads while
        // following the original curvature (see relaxQuadMesh).
        const ReferenceSurface sourceSurface(work, params.smoothNormalDegrees);
        if (!sourceSurface.empty()) {
            for (Index vi = 0; vi < result.mesh.vertexCapacity(); ++vi) {
                const VertexId v{vi};
                if (result.mesh.isAlive(v)) {
                    result.mesh.setPosition(v, sourceSurface.project(result.mesh.position(v)));
                }
            }
            relaxQuadMesh(result.mesh, sourceSurface, params.sharpEdgeDegrees,
                          /*iterations=*/20, /*lambda=*/0.5f);
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
