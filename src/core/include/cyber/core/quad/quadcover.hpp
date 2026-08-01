#pragma once

#include <memory>

#include "cyber/core/quad/stages.hpp"

namespace cyber::remesh::quad {

// QuadCover-based implementations of the field and parameterization stages,
// backed by the vendored geogram/exploragram slice (design D2 baseline).
// Both are safe to run concurrently on distinct islands from distinct
// threads: OpenNL's solver context and the exploragram progress hooks are
// thread_local in the vendored slice. The frame-field solve serializes
// internally on geogram's process-global progress task (a short section).
struct QuadCoverOptions {
    // Dihedral threshold forwarded to geogram's own feature detection (frame
    // field locking + quad_cover hard-edge constraints). Task 5.10 replaces
    // this angle re-detection with the explicit FeatureGraph as the single
    // source of truth.
    float sharpEdgeDegrees = 90.0f;
};

std::unique_ptr<IFrameFieldSolver> makeQuadCoverFrameFieldSolver(QuadCoverOptions options = {});
std::unique_ptr<IParameterizer> makeQuadCoverParameterizer(QuadCoverOptions options = {});

}  // namespace cyber::remesh::quad
