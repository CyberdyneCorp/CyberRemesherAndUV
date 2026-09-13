#include "remesh_adapter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cyber::capi::remesh_adapter {
namespace {

bool assignFloats(const float* source, std::size_t count, std::vector<float>& destination) {
    if (count > destination.max_size()) {
        return false;
    }
    destination.assign(source, source + count);
    return true;
}

void applyExecutionLimits(const CyberRemeshExecutionLimits& input, remesh::ResourceLimits& output) {
    output.maxDirectFactorBytes = static_cast<std::size_t>(input.maxDirectFactorBytes);
    output.maxCandidateBytes = static_cast<std::size_t>(input.maxCandidateBytes);
}

}  // namespace

LoweredRequest lowerRequest(const CyberRemeshParams& input, const CyberRemeshLimits* topologyLimits,
                            const CyberRemeshExecutionLimits* executionLimits) {
    LoweredRequest output;
    output.parameters.targetQuadCount = input.targetQuads;
    output.parameters.edgeScale = input.edgeScale;
    output.parameters.sharpEdgeDegrees = input.sharpEdgeDegrees;
    output.parameters.smoothNormalDegrees = input.smoothNormalDegrees;
    output.parameters.adaptivity = input.adaptivity;
    output.parameters.pureQuads = input.pureQuads != 0;
    output.parameters.holeFillMaxBoundary = input.holeFillMaxBoundary;
    if (topologyLimits != nullptr || executionLimits != nullptr) {
        output.limits =
            topologyLimits != nullptr
                ? remesh::ResourceLimits{static_cast<std::size_t>(topologyLimits->maxInputVertices),
                                         static_cast<std::size_t>(topologyLimits->maxInputFaces),
                                         static_cast<std::size_t>(
                                             topologyLimits->maxIntermediateVertices),
                                         static_cast<std::size_t>(
                                             topologyLimits->maxIntermediateFaces),
                                         static_cast<std::size_t>(
                                             topologyLimits->maxOutputVertices),
                                         static_cast<std::size_t>(topologyLimits->maxOutputFaces)}
                : remesh::ResourceLimits{};
        if (executionLimits != nullptr) {
            applyExecutionLimits(*executionLimits, *output.limits);
        }
    }
    return output;
}

bool lowerGuidance(const CyberGuidance& input, remesh::Guidance& output) {
    if ((input.guides == nullptr) != (input.guide_count == 0)) {
        return false;
    }
    for (size_t i = 0; i < input.guide_count; ++i) {
        const CyberFlowGuide& guideInput = input.guides[i];
        if (guideInput.points == nullptr && guideInput.point_count != 0) {
            return false;
        }
        remesh::FlowGuide guide;
        guide.strength = guideInput.strength;
        guide.radius = guideInput.radius;
        guide.points.reserve(guideInput.point_count);
        for (size_t point = 0; point < guideInput.point_count; ++point) {
            guide.points.push_back(Vec3{guideInput.points[3 * point],
                                        guideInput.points[3 * point + 1],
                                        guideInput.points[3 * point + 2]});
        }
        output.guides.push_back(std::move(guide));
    }
    if ((input.vertex_density == nullptr) != (input.vertex_density_count == 0) ||
        (input.face_density == nullptr) != (input.face_density_count == 0)) {
        return false;
    }
    return assignFloats(input.vertex_density, input.vertex_density_count,
                        output.density.vertexValues) &&
           assignFloats(input.face_density, input.face_density_count, output.density.faceValues);
}

bool lowerGuidanceEx(const CyberGuidanceEx& input, remesh::Guidance& output, std::string& reason) {
    if ((input.guides == nullptr) != (input.guide_count == 0)) {
        reason = "guide array pointer/count mismatch";
        return false;
    }
    for (size_t i = 0; i < input.guide_count; ++i) {
        const CyberFlowGuideEx& guideInput = input.guides[i];
        if (guideInput.points == nullptr && guideInput.point_count != 0) {
            reason = "guide " + std::to_string(i) + ": point array pointer/count mismatch";
            return false;
        }
        if (guideInput.mode != CYBER_GUIDE_ORIENTATION && guideInput.mode != CYBER_GUIDE_TOPOLOGY) {
            reason =
                "guide " + std::to_string(i) + ": unknown mode " + std::to_string(guideInput.mode);
            return false;
        }
        remesh::FlowGuide guide;
        guide.strength = guideInput.strength;
        guide.radius = guideInput.radius;
        guide.mode = guideInput.mode == CYBER_GUIDE_TOPOLOGY ? remesh::GuideMode::Topology
                                                             : remesh::GuideMode::Orientation;
        guide.closed = guideInput.closed != 0;
        guide.points.reserve(guideInput.point_count);
        for (size_t point = 0; point < guideInput.point_count; ++point) {
            guide.points.push_back(Vec3{guideInput.points[3 * point],
                                        guideInput.points[3 * point + 1],
                                        guideInput.points[3 * point + 2]});
        }
        output.guides.push_back(std::move(guide));
    }
    if ((input.vertex_density == nullptr) != (input.vertex_density_count == 0)) {
        reason = "vertex density array pointer/count mismatch";
        return false;
    }
    if ((input.face_density == nullptr) != (input.face_density_count == 0)) {
        reason = "face density array pointer/count mismatch";
        return false;
    }
    if (!assignFloats(input.vertex_density, input.vertex_density_count,
                      output.density.vertexValues)) {
        reason = "vertex density count is larger than any allocation could hold";
        return false;
    }
    if (!assignFloats(input.face_density, input.face_density_count, output.density.faceValues)) {
        reason = "face density count is larger than any allocation could hold";
        return false;
    }
    return true;
}

void writeZRemesherReport(const remesh::ZRemesherRunReport& input,
                          const remesh::SymmetryRunReport& symmetry, CyberZRemesherReport& output) {
    output = CyberZRemesherReport{};
    output.layouts = input.layout.layouts;
    output.layoutsValid = input.layout.layoutsValid;
    output.layoutNodes = input.layout.stats.nodes;
    output.layoutArcs = input.layout.stats.arcs;
    output.layoutPatches = input.layout.stats.patches;
    output.singularities = input.layout.stats.singularities;
    output.tJunctions = input.layout.stats.tJunctions;
    output.featureArcs = input.layout.stats.featureArcs;
    output.boundaryArcs = input.layout.stats.boundaryArcs;
    output.excludedArcs = input.layout.stats.excludedArcs;
    output.nonClosingPatches = input.layout.stats.nonClosingPatches;
    output.totalIndex = input.layout.stats.totalIndex;
    const std::size_t size =
        std::min(input.selectedCandidate.size(), sizeof(output.selectedCandidate) - 1);
    std::memcpy(output.selectedCandidate, input.selectedCandidate.data(), size);
    output.selectedCandidate[size] = '\0';
    output.qualityScore = input.qualityScore;
    output.symmetryApplied = symmetry.applied ? 1 : 0;
    output.topologicallySymmetric = symmetry.topologicallySymmetric ? 1 : 0;
    output.mirroredVertices = symmetry.mirroredVertices;
    output.mirroredFaces = symmetry.mirroredFaces;
    output.borderSnapped = symmetry.borderSnapped;
    output.membranesRemoved = symmetry.membranesRemoved;
    output.maxBorderDrift = symmetry.maxBorderDrift;
}

void writeInjectabilityReport(const remesh::ZRemesherRunReport& input,
                              CyberZRemesherInjectabilityReport& output) {
    output = CyberZRemesherInjectabilityReport{};
    const remesh::InjectabilityStats& stats = input.layout.injectability;
    output.arcs = stats.arcs;
    output.injectableArcs = stats.injectableArcs;
    output.excludedArcs = stats.excludedArcs;
    output.emptyRows = stats.emptyRows;
    output.latticeFreeRows = stats.latticeFreeRows;
    output.fractionalCoefficientRows = stats.fractionalCoefficientRows;
    output.fractionalPivotRows = stats.fractionalPivotRows;
    output.droppedRows = stats.droppedRows;
    output.pivots = stats.pivots;
    output.cleanPivots = stats.cleanPivots;
    output.injectedPivots = stats.injectedPivots;
    output.optimumDeviationEnergy = stats.optimumDeviationEnergy;
    output.realizedDeviationEnergy = stats.realizedDeviationEnergy;
}

bool writeSemanticBoundaryReport(const Mesh& output,
                                 const std::vector<remesh::SemanticBoundaryRequest>& requests,
                                 float targetEdgeLength, CyberSemanticBoundaryReport& report) {
    CyberSemanticBoundaryResult* const rows = report.boundaries;
    const size_t capacity = report.boundaryCapacity;
    report = CyberSemanticBoundaryReport{};
    report.boundaries = rows;
    report.boundaryCapacity = capacity;
    report.boundaryCount = requests.size();
    if ((rows == nullptr && capacity != 0) || (rows != nullptr && capacity < requests.size())) {
        return false;
    }
    const float tolerance = std::max(1e-4f, 0.5f * targetEdgeLength);
    for (size_t i = 0; i < requests.size(); ++i) {
        const remesh::SemanticBoundaryAdherence measured =
            remesh::measureSemanticBoundaryAdherence(output, requests[i], tolerance);
        CyberSemanticBoundaryResult& row = rows[i];
        row = CyberSemanticBoundaryResult{};
        std::snprintf(row.id, sizeof(row.id), "%s", measured.id.c_str());
        row.sourceEdges = measured.sourceEdges;
        row.requestedClosed = measured.requestedClosed ? 1 : 0;
        row.outputClosed = measured.outputClosed ? 1 : 0;
        row.edgeChainCoverage = measured.adherence.edgeChainCoverage;
        row.meanDistance = measured.adherence.meanDistance;
        row.maxDistance = measured.adherence.maxDistance;
        std::snprintf(row.reason, sizeof(row.reason), "%s", measured.reason.c_str());
        if (measured.realized) {
            row.state = CYBER_SEMANTIC_REALIZED;
            ++report.realizedCount;
        } else if (!requests[i].rejectionReason.empty()) {
            row.state = CYBER_SEMANTIC_REJECTED;
            ++report.rejectedCount;
        } else {
            row.state = CYBER_SEMANTIC_PARTIAL;
            ++report.partialCount;
        }
    }
    return true;
}

}  // namespace cyber::capi::remesh_adapter
