#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/snapping.hpp"

// Conform to an updated Target (pipeline-bridge spec, "Conform to an updated
// Target"): the sculpt changed after retopology started, so re-snap the
// existing EditMesh onto the new surface WITHOUT touching its connectivity, and
// report how far it had to move.
//
// The reporting is the point. A conform that silently stretches the cage is a
// competitor's bug we are deliberately not reproducing: every run carries the
// maximum AND the RMS deviation, and vertices past a caller-set threshold come
// back flagged for review — the operation still completes, it just refuses to
// be quiet about it.
namespace cyber::retopo {

struct ConformParams {
    // Deviation past which a vertex is flagged. <= 0 disables flagging (the
    // deviation figures are still reported).
    float deviationThreshold = 0.0f;
};

struct ConformReport {
    std::size_t movedVertices = 0;  // vertices the re-snap actually displaced
    float maxDeviation = 0.0f;
    float rmsDeviation = 0.0f;
    std::vector<VertexId> flagged;  // ascending id order
};

// Re-snaps every live, unpinned vertex of `mesh` onto `snapper`'s Target.
// Topology preservation is structural: the only mutation performed is
// Mesh::setPosition, so vertex/edge/face counts and every face's vertex list
// are untouched by construction.
//
// Pinned vertices are left exactly where they are AND excluded from the
// deviation statistics — a pin is the user saying "this one does not follow the
// Target", so counting its distance would report a deviation the conform never
// had the option of fixing.
[[nodiscard]] inline ConformReport conform(Mesh& mesh, const SurfaceSnapper& snapper,
                                           const ConformParams& params = {},
                                           const PinSet* pins = nullptr) {
    ConformReport report;
    if (snapper.empty()) {
        return report;
    }
    double sumSquared = 0.0;
    std::size_t considered = 0;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v) || (pins != nullptr && pins->isPinned(v))) {
            continue;
        }
        const Vec3 before = mesh.position(v);
        const SurfaceHit hit = snapper.snapToSurface(before);
        const float deviation = std::sqrt(std::fmax(0.0f, hit.distanceSquared));
        ++considered;
        sumSquared += static_cast<double>(deviation) * static_cast<double>(deviation);
        report.maxDeviation = std::fmax(report.maxDeviation, deviation);
        if (params.deviationThreshold > 0.0f && deviation > params.deviationThreshold) {
            report.flagged.push_back(v);
        }
        if (deviation > 0.0f) {
            mesh.setPosition(v, hit.point);
            ++report.movedVertices;
        }
    }
    if (considered > 0) {
        report.rmsDeviation =
            static_cast<float>(std::sqrt(sumSquared / static_cast<double>(considered)));
    }
    return report;
}

}  // namespace cyber::retopo
