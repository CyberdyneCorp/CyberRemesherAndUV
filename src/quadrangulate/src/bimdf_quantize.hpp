// Bi-MDF quantization of the native seamless parameterization
// (openspec/changes/bimdf-quantization). Two stages, both internal to the
// quadrangulate module:
//
//  1. buildTMesh: trace the motorcycle graph of the RELAXED seamless map
//     (real-valued seam translations, seam constraints exact) — separatrix
//     rays from every cone along the combed grid axes, transported through
//     seams, crashing into earlier rays / pinned crease chains / cones — and
//     assemble the T-mesh: nodes, arcs with relaxed lengths, quad patches.
//     Every arc also carries its EXACT symbolic length as a linear expression
//     over the promoted solver variables z = [u | v | t | c], so an integer
//     arc assignment can be mapped back onto the solver's free-integer basis.
//
//  2. solveBimdf: assign integer arc lengths by min-deviation flow
//     (Heistermann et al. 2023, "Min-Deviation-Flow in Bi-directed Graphs for
//     T-Mesh Quantization" — the S1 approximate path: split-node template,
//     T-join parity adjustment, asymmetric double cover, ordinary min-cost
//     flow). Implemented from the paper; the MIT-licensed libSatsuma serves
//     as a correctness reference only (no code vendored). GPL sources
//     (quadwild-bimdf integration code) remain untouched.
//
// The caller (solveSeamlessReduced) owns the reduction z = T w and performs
// the arc -> free-integer back-substitution and the actual injection.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cyber::remesh::bimdf {

// Sparse linear expression over the promoted solver variables z
// (index -> coefficient). All constraints are homogeneous, so there is no
// constant term anywhere.
using ZExpr = std::unordered_map<std::size_t, double>;

// One seam (cut) edge of the cut-open mesh, mirroring the solver's SeamRef
// plus the data the tracer needs: the two incident compact face indices and
// the promoted z indices of this edge's integer translation.
struct SeamEdge {
    std::size_t aA = 0, bA = 0, aB = 0, bB = 0;  // cut-vertex ids, side A/B
    int rho = 0;                                 // uv_B = R^rho uv_A + t, R = CCW (u,v)->(-v,u)
    bool feature = false;  // pinned crease: coordinate pinAxis constant+integer on side A
    int pinAxis = 0;       // 0=u, 1=v (side-A chart)
    std::size_t faceA = 0, faceB = 0;  // compact face indices (Charts::faces)
    std::size_t tx = 0, ty = 0;        // promoted z indices of the translation
};

// The cut-open relaxed-seamless parameterization, in compact form.
struct Charts {
    std::size_t nCut = 0;
    const float* u = nullptr;  // relaxed-seamless UV per cut vertex (z = Tuv w)
    const float* v = nullptr;
    std::vector<std::array<std::size_t, 3>> faces;  // per-face cut-vertex ids
    std::vector<int> coneIndex;                     // per cut vertex, 0 = regular
    std::vector<std::uint32_t> vertexOfCut;         // mesh vertex id per cut vertex
    std::vector<SeamEdge> seams;
    // Intrinsic (fold-proof) geometry for separatrix launch selection: mesh
    // positions per MESH vertex id, and the combed frame per compact face
    // (e0 = chart +u direction in 3D, n = unit normal).
    std::vector<std::array<float, 3>> vertexPos;
    std::vector<std::array<float, 3>> faceE0;
    std::vector<std::array<float, 3>> faceN;

    [[nodiscard]] std::size_t uIx(std::size_t c) const { return c; }
    [[nodiscard]] std::size_t vIx(std::size_t c) const { return nCut + c; }
};

struct Arc {
    std::size_t n0 = 0, n1 = 0;  // node ids
    double len = 0.0;            // relaxed length in grid cells (>= 0)
    ZExpr expr;                  // symbolic length over z; expr(z) == +len at the relaxed z
    bool onFeature = false;      // runs along a pinned crease chain
};

struct Patch {
    // Arc ids per side, in boundary order; side 0 is opposite side 2, 1 opposite 3.
    std::array<std::vector<std::size_t>, 4> side;
};

struct TMesh {
    bool ok = false;
    std::string reason;  // failure diagnostic when !ok
    std::size_t nodeCount = 0;
    std::vector<Arc> arcs;
    std::vector<Patch> patches;
    // Diagnostics.
    std::size_t coneNodes = 0, creaseNodes = 0, tNodes = 0;
    std::size_t raysTraced = 0, raySteps = 0;
    double maxExprErr = 0.0;        // max |expr(z_relaxed) - len| over arcs
    double maxSideMismatch = 0.0;   // max |sum(side k) - sum(side k+2)| relaxed
    double minArcLen = 0.0;         // most negative relaxed arc length (fold diagnostic)
    std::size_t prunedChains = 0;   // stub crease chains dropped (phase-only pins)
    std::size_t failedRays = 0;     // separatrix launches abandoned at folded cones
    std::size_t degradedNodes = 0;  // nodes with fold-damaged sector windings
    std::size_t repairedNodes = 0;  // fold-damaged fans fixed by signed-angle sectors
};

// zValue(i) must return the relaxed value of promoted variable i (used only
// for the expr(z) == len self-check diagnostics).
TMesh buildTMesh(const Charts& charts, const std::vector<double>& zRelaxed);

struct BimdfResult {
    bool ok = false;
    std::string reason;
    // Assigned quantized arc lengths, in HALF-CELL units (cones live on the
    // half-integer lattice under the solver's reduction, so arc lengths are
    // integers in half-cells). arcLenHalf[a] >= 1.
    std::vector<long long> arcLenHalf;
    double deviationEnergy = 0.0;    // sum_a |x_a/2 - len_a| / max(len_a, 0.5)
    std::size_t raisedToMin = 0;     // degenerate-assignment guard adjustments
    std::size_t parityFlips = 0;     // T-join adjustments applied
    std::size_t halfIntegral = 0;    // cover edges whose mapped-back flow was odd
    long long coverCost = 0;         // scaled min-cost-flow objective (diagnostic)
    long long maxSideViolation = 0;  // worst |side sum - opposite side sum| (half-cells)
};

// Bi-MDF S1 solve over the T-mesh (targets from Arc::len, consistency from
// the patches). Pure graph computation; no solver state involved.
BimdfResult solveBimdf(const TMesh& tm);

// Deviation energy of an arbitrary half-cell assignment against the relaxed
// targets — the same functional solveBimdf minimizes, evaluable for the
// greedy assignment for A/B reporting.
double deviationEnergy(const TMesh& tm, const std::vector<double>& arcLenCells);

}  // namespace cyber::remesh::bimdf
