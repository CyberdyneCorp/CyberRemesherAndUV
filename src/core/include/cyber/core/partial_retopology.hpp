#pragma once

#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"

namespace cyber::remesh {

// Exact partial retopology keeps the exterior mesh and this loop's source
// vertex ids untouched. The first solver supports the four-edge loop that its
// deterministic five-quad patch construction can prove correct.
enum class PartialRetopologyStatus { Ready, Rejected };

struct PartialRetopologyRequest {
    std::vector<FaceId> faces;
    bool allQuadsRequired = true;
};

struct PartialRetopologyAnalysis {
    PartialRetopologyStatus status = PartialRetopologyStatus::Rejected;
    std::vector<FaceId> selectedFaces;
    // Ordered without repeating the first vertex. The selected region is on
    // the left of every directed edge.
    std::vector<VertexId> boundary;
    std::string reason;
};

// Validates a selected region without mutating `source`. Ready means one
// connected selected component bounded by one simple, closed, manifold,
// four-edge interior loop; all other forms are rejected with a reason.
[[nodiscard]] PartialRetopologyAnalysis analyzePartialRetopology(
    const Mesh& source, const PartialRetopologyRequest& request);

}  // namespace cyber::remesh
