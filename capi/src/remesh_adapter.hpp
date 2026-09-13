#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/remesh_params.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"
#include "cyber/quadrangulate/seamless_solver.hpp"
#include "cyber/quadrangulate/symmetry_layout.hpp"
#include "cyber/quadrangulate/topology_guides.hpp"
#include "cyber_capi.h"

namespace cyber::capi::remesh_adapter {

// Data owned by a single C ABI invocation after caller POD has been lowered.
// It deliberately excludes mesh and callback ownership, which remain at the
// exported boundary.
struct LoweredRequest {
    remesh::Parameters parameters;
    std::optional<remesh::ResourceLimits> limits;
};

[[nodiscard]] LoweredRequest lowerRequest(const CyberRemeshParams& parameters,
                                          const CyberRemeshLimits* topologyLimits,
                                          const CyberRemeshExecutionLimits* executionLimits);

[[nodiscard]] bool lowerGuidance(const CyberGuidance& input, remesh::Guidance& output);
[[nodiscard]] bool lowerGuidanceEx(const CyberGuidanceEx& input, remesh::Guidance& output,
                                   std::string& reason);

void writeZRemesherReport(const remesh::ZRemesherRunReport& remeshReport,
                          const remesh::SymmetryRunReport& symmetryReport,
                          CyberZRemesherReport& output);
void writeInjectabilityReport(const remesh::ZRemesherRunReport& remeshReport,
                              CyberZRemesherInjectabilityReport& output);
[[nodiscard]] bool writeSemanticBoundaryReport(
    const Mesh& output, const std::vector<remesh::SemanticBoundaryRequest>& requests,
    float targetEdgeLength, CyberSemanticBoundaryReport& report);

}  // namespace cyber::capi::remesh_adapter
