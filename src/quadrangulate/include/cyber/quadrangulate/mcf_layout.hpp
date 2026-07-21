#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/position_field.hpp"

// Min-cost-flow integer layout (docs/mcf-integer-layout-plan.md) — the port of
// QuadriFlow's integer-layout stage onto the vendored, dependency-free SciPP
// max-flow. M1 = the SciPP linkage self-test; M2 = the per-edge integer-difference
// representation (pure integer math, no SciPP); M3+ add the flow solve.
namespace cyber::remesh {

// Runs SciPP's csgraph::maximum_flow on a tiny known network (max-flow == 5) and
// returns true iff it computes the expected value. Returns false when the module
// was built without CYBER_WITH_SCIPP. Exercised by the gated unit test.
[[nodiscard]] bool mcfMaxFlowSelfTest();

// A 2D integer lattice offset (u,v steps), QuadriFlow's Vector2i.
struct Vec2i {
    int x = 0;
    int y = 0;
    friend bool operator==(Vec2i a, Vec2i b) { return a.x == b.x && a.y == b.y; }
};

// QuadriFlow-style per-edge integer-difference layout (M2), built from the
// position field (orientation q + lattice position o). This is the representation
// the M3 min-cost flow operates on: `edgeDiff[e]` is the integer number of lattice
// cells edge e spans in (u,v); a face whose three edge diffs do not sum to zero is
// a position singularity, and the flow makes the non-singular faces consistent.
struct McfEdgeInfo {
    std::vector<Vec2i> edgeDiff;                       // per unique edge: integer lattice diff
    std::vector<std::pair<Index, Index>> edgeValues;   // per edge: endpoints (canonical, v1<v2)
    std::vector<std::array<int, 3>> faceEdgeIds;       // per compact face: its 3 edge ids
    std::vector<Index> faceList;                        // compact face index -> mesh FaceId value
    std::unordered_map<int, Vec2i> posSingularities;   // compact face -> position singularity index
    std::unordered_map<int, int> orientSingularities;  // compact face -> orientation index (mod 4)
    bool valid = false;
};

// Build the QuadriFlow-style edge_diff representation from a triangle mesh and its
// position field. Ports ComputeOrientationSingularities + ComputePositionSingularities
// + BuildEdgeInfo (parametrizer-sing.cpp, parametrizer-int.cpp). Pure integer/vector
// math — no SciPP dependency. See docs/mcf-integer-layout-plan.md (M2).
[[nodiscard]] McfEdgeInfo buildMcfEdgeInfo(const Mesh& mesh, const PositionField& field);

}  // namespace cyber::remesh
