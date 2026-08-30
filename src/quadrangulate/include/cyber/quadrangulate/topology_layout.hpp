#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Explicit topology layout — the ZRemesher-class retopology stack's first-class
// intermediate artifact (openspec/changes/add-zremesher-retopology, Phase A).
//
// Today's quad topology is an EMERGENT consequence of cross field + seamless
// grid + isoline extraction. That is mathematically sound but leaves nothing to
// reason about when the question is "where should the edge loops go" — which is
// the question an artist-facing retopologizer has to answer. `TopologyLayout`
// makes that structure explicit:
//
//     nodes   — singularities, feature/boundary corners, T-junctions, and
//               (later) guide and symmetry anchors;
//     arcs    — the separatrix / feature / boundary curves between nodes,
//               carrying their traced 3D polyline and their continuous and
//               quantized lengths;
//     patches — the regions those arcs bound, with their boundary in sides.
//
// Division of labour with `bimdf::TMesh`: the T-mesh is the QUANTIZATION view
// of the same graph — it additionally carries each arc's exact symbolic length
// as a linear expression over the seamless solver's promoted variables, which
// is meaningless outside the integer solve. `TopologyLayout` is the GEOMETRIC
// and COMBINATORIAL view: no solver variables, so it can be scored, validated,
// exported, mirrored, and constrained by guides without dragging the solver in.
// Arc and node ids are shared between the two, so a stage may hold both.
//
// Building a layout never changes the quantized result: it is derived from the
// traced T-mesh, and the quantizer keeps consuming the T-mesh.
namespace cyber::remesh {

using LayoutNodeId = std::uint32_t;
using LayoutArcId = std::uint32_t;
using LayoutPatchId = std::uint32_t;

inline constexpr LayoutNodeId kInvalidLayoutId = 0xFFFFFFFFu;

enum class LayoutNodeKind : std::uint8_t {
    // A cross-field cone: an extraordinary vertex of the final quad mesh.
    Singularity,
    // Corner of an open-surface boundary loop.
    BoundaryCorner,
    // Junction on a pinned crease chain.
    FeatureCorner,
    // Interior point where one separatrix crashes into another. Not an
    // extraordinary vertex — it is a valence-4 node of the layout graph
    // whose four sides need not carry equal quantized lengths.
    TJunction,
    // Anchor of an artist topology guide (Phase E).
    GuideAnchor,
    // Anchor on a symmetry plane (Phase F).
    SymmetryAnchor,
    // A regular mesh vertex a trace happened to land on exactly.
    Regular
};

enum class LayoutArcKind : std::uint8_t { Separatrix, Feature, Boundary, Guide, Symmetry };

struct LayoutNode {
    LayoutNodeId id = kInvalidLayoutId;
    LayoutNodeKind kind = LayoutNodeKind::TJunction;

    Vec3 position{};
    // Source mesh vertex when the node sits on one; invalid for T-junctions.
    VertexId vertex{};
    // Source face the node was traced in (always valid).
    FaceId face{};

    // Cross-field index for singularities; 0 for every other kind.
    int singularityIndex = 0;

    // Locked nodes may not be relocated by later optimization stages.
    bool locked = false;
};

struct LayoutSample {
    Vec3 position{};
    FaceId face{};
};

struct LayoutArc {
    LayoutArcId id = kInvalidLayoutId;
    LayoutNodeId begin = kInvalidLayoutId;
    LayoutNodeId end = kInvalidLayoutId;
    LayoutArcKind kind = LayoutArcKind::Separatrix;

    // Traced polyline from `begin` to `end`, both endpoints included. May be
    // empty when the tracer ran without geometry capture.
    std::vector<LayoutSample> samples;

    // Continuous length in grid cells, from the relaxed parameterization.
    double desiredLength = 0.0;
    // Integer grid edges assigned by quantization; < 0 until quantized.
    int quantizedLength = -1;

    // The arc lies in a region the tracer could not validate and which the
    // quantizer therefore excludes from injection.
    bool excluded = false;
    // Quantization may not move this arc (pinned crease, guide, symmetry).
    bool locked = false;
};

struct LayoutPatch {
    LayoutPatchId id = kInvalidLayoutId;

    // Boundary arcs grouped into logical sides, in boundary order. A quad-like
    // patch has exactly four sides, with side 0 opposite side 2.
    std::vector<std::vector<LayoutArcId>> sides;

    // Quantized dimensions; < 0 until quantized.
    int uCount = -1;
    int vCount = -1;

    [[nodiscard]] bool quadLike() const { return sides.size() == 4; }
    [[nodiscard]] std::size_t arcCount() const;
};

// Aggregate counts, cheap to compute and stable across runs. Reported in the
// run report and asserted by the benchmark gates.
struct LayoutStats {
    std::size_t nodes = 0;
    std::size_t arcs = 0;
    std::size_t patches = 0;
    std::size_t singularities = 0;
    std::size_t tJunctions = 0;
    std::size_t boundaryArcs = 0;
    std::size_t featureArcs = 0;
    std::size_t excludedArcs = 0;
    std::size_t nonQuadPatches = 0;
    // Patches whose boundary walk did not close; 0 until validated.
    std::size_t nonClosingPatches = 0;
    // Sum of singularity indices; 4 * Euler characteristic for a valid field.
    int totalIndex = 0;
};

struct TopologyLayout {
    std::vector<LayoutNode> nodes;
    std::vector<LayoutArc> arcs;
    std::vector<LayoutPatch> patches;

    // Set from a LayoutValidation by the caller; false until validated.
    bool valid = false;
    // The hard violation when !valid, empty otherwise.
    std::string invalidReason;
    // Patches validation found non-closing, in id order. These are contained,
    // not fatal: they are excluded from injection like a rejected orbit.
    std::vector<LayoutPatchId> nonClosingPatches;

    [[nodiscard]] std::size_t singularityCount() const;
    [[nodiscard]] std::size_t nonQuadPatchCount() const;
    [[nodiscard]] LayoutStats stats() const;
};

// Result of checking a layout against the Appendix-B invariants.
//
// The two failure classes are deliberately separate, mirroring how the tracer
// already treats them. A HARD violation — a bad id, a non-finite position, an
// arc pointing at a missing node — means the graph itself is corrupt and
// nothing may consume it. A patch whose boundary walk does not close is a LOCAL
// failure: the rest of the layout is sound, and the offending region is
// contained (excluded from injection) exactly like a rejected orbit.
struct LayoutValidation {
    // No hard structural violation. Local (per-patch) failures may still be
    // listed below.
    bool ok = false;
    // The first hard violation, in a fixed check order; empty when ok.
    std::string error;
    // Patches whose boundary arcs do not form a closed walk, in id order.
    std::vector<LayoutPatchId> nonClosingPatches;

    // Nothing wrong at all: safe to consume every patch.
    [[nodiscard]] bool clean() const { return ok && nonClosingPatches.empty(); }
};

// Check a layout against its combinatorial invariants (design Appendix B).
// Deterministic: the same layout always reports the same first violation and
// the same patch list.
//
// `faceCount` bounds node and sample face ids — the source mesh's FaceId
// CAPACITY, since ids stay sparse after deletions. Pass 0 to skip that check.
[[nodiscard]] LayoutValidation validateTopologyLayout(const TopologyLayout& layout,
                                                      std::size_t faceCount);

// Debug export. Both are byte-reproducible for a given layout: no pointers, no
// hash iteration order, fixed-precision floats.
//
// JSON: stats plus every node, arc and patch. Machine-readable, consumed by the
// benchmark harness and the examples.
[[nodiscard]] std::string layoutToJson(const TopologyLayout& layout);

// OBJ: arc polylines as `l` elements and nodes as isolated vertices, so the
// layout can be dropped straight into a 3D viewer next to the output mesh.
// Node vertices come first (one per node, in id order) so a node's OBJ index is
// its id + 1.
[[nodiscard]] std::string layoutToObj(const TopologyLayout& layout);

}  // namespace cyber::remesh
