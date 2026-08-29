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
    // Relaxed-seamless UV per cut vertex (z = Tuv w). Double so that exactly
    // coincident separatrix levels (twin launches) stay coincident: a float
    // substrate breaks them apart by one ulp, which the tracer's collinear-
    // overlap hazard band rejects.
    const double* u = nullptr;
    const double* v = nullptr;
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
    // Source mesh FaceId per compact face. Only needed to report layout
    // geometry back in the caller's index space; may stay empty.
    std::vector<std::uint32_t> faceOfCompact;
    // FaceId capacity of the SOURCE mesh, so layout face ids can be
    // range-checked in the index space faceOfCompact maps into (ids are sparse
    // after deletions, so this is the capacity, not the alive count). 0 when
    // unknown.
    std::size_t sourceFaceCount = 0;
    // Capture the traced geometry (node positions, arc polylines) into
    // TMesh::nodeGeom / TMesh::arcGeom. Write-only with respect to the
    // quantizer, so it cannot change an assignment — see the geometry note on
    // TMesh below.
    bool captureGeometry = false;

    [[nodiscard]] std::size_t uIx(std::size_t c) const { return c; }
    [[nodiscard]] std::size_t vIx(std::size_t c) const { return nCut + c; }
};

struct Arc {
    std::size_t n0 = 0, n1 = 0;  // node ids
    double len = 0.0;            // relaxed length in grid cells (>= 0)
    ZExpr expr;                  // symbolic length over z; expr(z) == +len at the relaxed z
    bool onFeature = false;      // runs along a pinned crease chain
    // QGP-style boundary arc: a piece of an open-surface boundary loop between
    // T-nodes where separatrices land. It enters its single incident patch
    // side with a min-one floor; in the flow network it is a FIXED constant —
    // a variable encoding (paired through a balance-free cover hub) was
    // measured to produce bit-identical assignments while adding the
    // copy-crossing structure round 3 found half-integral. The free boundary
    // is not grid-aligned, so no exact symbolic length exists (noExpr):
    // boundary arcs are rounded by the greedy schedule, never injected or
    // steered directly.
    bool onBoundary = false;
    bool noExpr = false;  // no exact symbolic length (boundary-terminated)
};

struct Patch {
    // Arc ids per side, in boundary order. Quads (4 sides): side 0 is
    // opposite side 2, 1 opposite 3, quantized with the split-node template.
    // Polygonal patches (3-6 sides, one interior irregular vertex) are
    // quantized with the even-sum interior-routing template (Heistermann et
    // al. 2023, Sec. 4.4).
    std::vector<std::vector<std::size_t>> side{4};

    [[nodiscard]] bool isQuad() const { return side.size() == 4; }
};

// Geometric companion to the T-mesh, filled only when Charts::captureGeometry
// is set. NOTHING in solveBimdf reads it: the quantizer works off Arc::len,
// Arc::expr and the patches alone, so capturing geometry cannot move an
// assignment. It exists so the graph can leave this file as a
// `cyber::remesh::TopologyLayout` (topology_layout.hpp) that later stages —
// singularity scoring, guides, symmetry, export — can consume without
// dragging the solver's symbolic side along.
enum class NodeGeomKind : std::uint8_t {
    Cone,       // cross-field singularity
    Crease,     // vertex on a pinned crease chain
    Boundary,   // vertex on an open-surface boundary loop
    TJunction,  // interior point where separatrices / chains meet
    Regular     // plain mesh vertex a trace landed on exactly
};

struct NodeGeom {
    NodeGeomKind kind = NodeGeomKind::TJunction;
    int coneIndex = 0;
    // Mesh vertex the node sits on; 0xFFFFFFFF for T-junctions.
    std::uint32_t meshVertex = 0xFFFFFFFFu;
    // A compact face containing the node (always set).
    std::size_t face = 0;
    std::array<float, 3> position{};
};

struct ArcPoint {
    std::array<float, 3> position{};
    std::size_t face = 0;  // compact face the point lies in
};

struct ArcGeom {
    // Traced polyline from Arc::n0 to Arc::n1, both endpoints included. Empty
    // when capture was off, or when the arc's trail was invalidated by a
    // jittered retry after the arc had already been recorded.
    std::vector<ArcPoint> points;
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
    std::size_t twinMerges = 0;     // coincident twin-arc pairs merged (bigon collapse)
    std::size_t spurCollapses = 0;  // dangling out-and-back arcs removed
    // Local containment: an orbit that fails validation (or touches a cone
    // with abandoned launches) no longer refuses the whole T-mesh. Its
    // region is EXCLUDED: arcs bounded only by rejected orbits are barred
    // from injection and fall back to the greedy rounding locally, while the
    // valid remainder of the T-mesh proceeds.
    std::size_t excludedPatches = 0;  // rejected orbits (locally contained)
    std::string rejectSummary;        // corner histogram of rejected orbits
    std::vector<char> arcExcluded;    // per (compact) arc: no accepted orbit covers it
    // Traced geometry (Charts::captureGeometry). nodeGeom is indexed by node
    // id and sized nodeCount; arcGeom is parallel to `arcs`. Both empty when
    // capture was off.
    std::vector<NodeGeom> nodeGeom;
    std::vector<ArcGeom> arcGeom;
};

// zValue(i) must return the relaxed value of promoted variable i (used only
// for the expr(z) == len self-check diagnostics).
TMesh buildTMesh(const Charts& charts, const std::vector<double>& zRelaxed);

struct BimdfResult {
    bool ok = false;
    std::string reason;
    // Assigned quantized arc lengths, in HALF-CELL units (cones live on the
    // half-integer lattice under the solver's reduction, so arc lengths are
    // integers in half-cells). arcLenHalf[a] >= 0; the anti-collapse floor
    // of one half-cell lives on the patch SIDES, not on individual arcs.
    std::vector<long long> arcLenHalf;
    double deviationEnergy = 0.0;    // sum_a |x_a/2 - len_a| / max(len_a, 0.5)
    std::size_t raisedToMin = 0;     // degenerate-assignment guard adjustments
    std::size_t parityFlips = 0;     // T-join adjustments applied
    std::size_t halfIntegral = 0;    // cover edges whose mapped-back flow was odd
    std::size_t polyPatches = 0;     // non-quad patches quantized via even-sum template
    std::size_t polyOddSum = 0;      // polygonal patches whose boundary sum came out odd
                                     // (unquantizable under the template; diagnostic)
    long long coverCost = 0;         // scaled min-cost-flow objective (diagnostic)
    long long maxSideViolation = 0;  // worst |side sum - opposite side sum| (half-cells)
    // Local-infeasibility safety valve: patches whose cover demand could not
    // be satisfied (fixed boundary sides pinched between excluded regions)
    // are dropped and the solve retried without them.
    std::size_t droppedPatches = 0;
    std::vector<char> arcOutside;  // per arc: outside the final network (never inject)
};

// Bi-MDF S1 solve over the T-mesh (targets from Arc::len, consistency from
// the patches). Pure graph computation; no solver state involved.
struct BimdfOptions {
    // Anti-collapse floor of one half-cell on single-arc patch sides. ON for
    // the injection solve (thin CAD strips must not collapse). The guided
    // rounding consults a floors-OFF solve instead: the floors were measured
    // to inflate the assignment past the greedy realization (nefertiti@4000
    // energy 2711 vs greedy 1879), leaving nothing worth steering toward,
    // while the floor-free flow decides open-vs-closed per strip honestly.
    bool sideFloors = true;
};
BimdfResult solveBimdf(const TMesh& tm, const BimdfOptions& opts = {});

// Deviation energy of an arbitrary half-cell assignment against the relaxed
// targets — the same functional solveBimdf minimizes, evaluable for the
// greedy assignment for A/B reporting.
double deviationEnergy(const TMesh& tm, const std::vector<double>& arcLenCells);

}  // namespace cyber::remesh::bimdf
