// Bridge from the Bi-MDF tracer's T-mesh to the reusable TopologyLayout
// (openspec/changes/add-zremesher-retopology, Phase A1).
//
// The tracer is where the layout is actually discovered — separatrix walks,
// crease chains, boundary chains, orbit extraction — and it is inseparable from
// the seamless solver's symbolic machinery. Rather than move 3000 lines of
// tracing out of `bimdf_quantize.cpp`, the split is by RESPONSIBILITY:
//
//   bimdf::TMesh       the quantization view — arc lengths, exact symbolic
//                      lengths over the solver's promoted variables, patch
//                      sides. Consumed by solveBimdf and nothing else.
//   TopologyLayout     the geometric/combinatorial view — node positions and
//                      kinds, arc polylines, patch sides. No solver variables,
//                      so every later stage (singularity scoring, guides,
//                      symmetry, scoring, export) consumes this instead.
//
// Node and arc ids are shared, so a stage holding both can cross-reference.
// Building the layout reads the T-mesh and writes nothing back: quantization is
// bit-for-bit unaffected.
#pragma once

#include "bimdf_quantize.hpp"
#include "cyber/quadrangulate/topology_layout.hpp"

namespace cyber::remesh {

// Promote a traced T-mesh to a layout. `charts` supplies the compact-face ->
// source FaceId map (Charts::faceOfCompact); when it is empty the layout
// reports compact face indices instead. Requires Charts::captureGeometry to
// have been set for the trace, otherwise arcs come back without samples and
// nodes without positions.
[[nodiscard]] TopologyLayout layoutFromTMesh(const bimdf::Charts& charts, const bimdf::TMesh& tm);

// Record a Bi-MDF assignment onto a layout built from the same T-mesh: arc
// quantized lengths in whole grid edges, and per-patch u/v counts. Arcs the
// solve left outside the network keep quantizedLength == -1.
void applyQuantization(TopologyLayout& layout, const bimdf::TMesh& tm,
                       const bimdf::BimdfResult& result);

}  // namespace cyber::remesh
