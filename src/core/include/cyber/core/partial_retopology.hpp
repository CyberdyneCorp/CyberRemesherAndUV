#pragma once

#include <array>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"

namespace cyber::remesh {

// Exact partial retopology keeps the exterior mesh and this loop's source
// vertex ids untouched. The first solver supports the four-edge loop that its
// deterministic five-quad patch construction can prove correct.
enum class PartialRetopologyStatus { Ready, Applied, Rejected };

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

// Maps a generated vertex to one source triangle. The source triangle is
// explicit because source faces may be n-gons and therefore do not have one
// unambiguous face-wide barycentric coordinate system.
struct SourceCorrespondence {
    VertexId outputVertex;
    FaceId sourceFace;
    std::array<VertexId, 3> sourceTriangle;
    Vec3 barycentric;
    float distance = 0.0f;
    float confidence = 0.0f;
};

// A region replacement is transactional: on rejection `mesh` is an unchanged
// copy of `source`; on success it shares every exterior element id and
// position with `source`.
struct PartialRetopologyResult {
    PartialRetopologyStatus status = PartialRetopologyStatus::Rejected;
    Mesh mesh;
    PartialRetopologyAnalysis analysis;
    std::vector<SourceCorrespondence> correspondences;
    std::string reason;
};

// Validates a selected region without mutating `source`. Ready means one
// connected selected component bounded by one simple, closed, manifold,
// four-edge interior loop; all other forms are rejected with a reason.
[[nodiscard]] PartialRetopologyAnalysis analyzePartialRetopology(
    const Mesh& source, const PartialRetopologyRequest& request);

// Replaces an accepted four-edge selected region with a deterministic five
// quad patch. This is the first exact-border solver: it deliberately rejects
// regions outside the analyzed contract rather than changing the exterior.
[[nodiscard]] PartialRetopologyResult partialRetopologize(const Mesh& source,
                                                          const PartialRetopologyRequest& request);

}  // namespace cyber::remesh
