#pragma once

#include <cstddef>
#include <vector>

#include "cyber/core/guidance.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/topology_layout.hpp"

// Artist topology guides (openspec/changes/add-zremesher-retopology, Phase E).
//
// An ORIENTATION guide biases the cross field near a stroke — the loops lean
// toward it, none of them is pinned to it. A TOPOLOGY guide asks for something
// stronger: an actual edge loop along the stroke. Getting that means the stroke
// has to stop being a soft field bias and become a CURVE ON THE MESH the solve
// treats as structure.
//
// The mechanism reuses what the engine already does for creases. A pinned
// crease is exactly "run an edge loop along this curve": the seamless solve
// makes it a hard seam and pins its isolines, so quads follow it. A topology
// guide is projected to a connected edge path and handed to that same
// machinery, rather than growing a parallel one that would need its own
// calibration.
namespace cyber::remesh {

struct GuidePath {
    // Mesh vertices the guide snapped to, in order. Consecutive entries are
    // connected by an edge.
    std::vector<VertexId> vertices;
    // The edges between them, which are what gets tagged.
    std::vector<EdgeId> edges;
    // The path returned to its start (a loop guide that closed successfully).
    bool closed = false;
    // How far the projected path strays from the requested polyline, in world
    // units — the honest measure of how well the mesh could represent the
    // stroke at all. A coarse mesh cannot follow a tight curve, and saying so
    // is better than silently producing a path that wanders.
    float maxDeviation = 0.0f;
};

// Project a guide onto the mesh as a connected edge path: snap each guide point
// to its nearest vertex, then join consecutive snaps by shortest edge paths.
//
// Deterministic — the shortest-path search breaks ties on vertex id, so the same
// guide on the same mesh always yields the same path. Returns an empty path when
// the guide has fewer than two points or the mesh cannot connect the snaps.
[[nodiscard]] GuidePath projectGuideToPath(const Mesh& mesh, const FlowGuide& guide);

// Insert a projected guide into a layout as first-class arcs: a GuideAnchor
// node per snapped vertex and a Guide arc between consecutive ones, so later
// stages see the guide as part of the topology rather than as an external hint.
// The layout's existing nodes and arcs are untouched; ids continue from the end.
void insertGuideArcs(TopologyLayout& layout, const GuidePath& path);

// How well an output mesh honours a guide, measured on the RESULT rather than
// promised by the input.
//
// `edgeChainCoverage` is the fraction of the guide that has an output edge both
// NEAR it and ALIGNED with it. Both halves are load-bearing. Proximity alone
// counts an edge that merely crosses the guide at right angles, which every
// dense mesh has everywhere — a metric built on that would report a high score
// for a mesh that ignored the stroke completely. Alignment alone would count an
// edge running parallel on the far side of the model.
struct GuideAdherence {
    float edgeChainCoverage = 0.0f;
    float meanDistance = 0.0f;
    float maxDistance = 0.0f;
    std::size_t samples = 0;
};

// `tolerance` is the distance within which an edge counts as on the guide;
// `maxAngleDegrees` is how far it may deviate from the guide's local tangent.
[[nodiscard]] GuideAdherence measureGuideAdherence(const Mesh& output, const FlowGuide& guide,
                                                   float tolerance, float maxAngleDegrees = 45.0f);

}  // namespace cyber::remesh
