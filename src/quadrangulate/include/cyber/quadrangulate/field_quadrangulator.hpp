#pragma once

#include <memory>

#include "cyber/core/quadrangulate.hpp"

namespace cyber::remesh {

// Cross-field-guided quadrangulator (QuadCover-lite). It computes a smoothed
// 4-symmetry field (see crossfield.hpp), then pairs triangles into quads
// preferring merges whose removed edge is diagonal to the field — so the
// surviving quad edges follow the field's flow — and finally simplifies the
// quad graph by dissolving valence-2 doublets (task 5.5). This is a genuine
// field-aligned quadrangulation without the full seamless MIQ parameterization
// / integer-isoline extraction of exploragram's QuadCover.
//
// Implements the core IQuadrangulator seam; the pipeline uses it when injected
// (the CLI does), while the default remains greedy pairing so recorded golden
// baselines stay stable.
std::unique_ptr<IQuadrangulator> makeFieldAlignedQuadrangulator(int fieldIterations = 30);

}  // namespace cyber::remesh
