#pragma once

#include <span>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/uv/packing.hpp"
#include "cyber/uv/seams.hpp"
#include "cyber/uv/unwrap.hpp"

// Automatic UV atlas generation (uv-editing spec, "Automatic unwrap"). The
// interactive tools in seams.hpp / unwrap.hpp / packing.hpp build an atlas from
// hand-drawn seams; this module supplies the non-interactive path: it seams a
// mesh by growing normal-coherent charts (the "Smart UV Project" family), LSCM-
// unwraps each chart, packs them into the unit square and writes the per-loop
// "uv" attribute — mesh in, packed atlas out.
namespace cyber::uv {

struct AtlasOptions {
    // A face joins a growing chart while the angle between its normal and the
    // chart's seed normal stays within this bound (degrees). Smaller values
    // make more, flatter charts (lower angular distortion, more seams).
    float maxChartAngleDeg = 40.0f;
    // Merge adjacent charts whose union still fits within a `maxChartAngleDeg`
    // normal cone. Greedy seed growth fragments bumpy surfaces into many small
    // charts that share a compatible orientation; merging them cuts seam length
    // (and packs better) without exceeding the per-chart flatness the growth
    // already guarantees, so distortion does not rise.
    bool mergeCharts = true;
    // Second, looser merge pass: after the cone merge, keep merging adjacent
    // charts as long as LSCM-unwrapping the union keeps its maximum conformal
    // (angle) error at or below this cap. Spends distortion headroom to cut the
    // chart/seam count toward what aggressive reference packers achieve. 0
    // disables it (cone merge only). Only consulted when `mergeCharts` is true.
    //
    // COST: this pass trial-unwraps the union of candidate chart pairs, so it
    // dominates the run — seconds to minutes on meshes of tens of thousands of
    // faces, where the rest of the atlas takes milliseconds. Callers that need a
    // bounded unwrap set this to 0; callers that keep it should pass a
    // CancelToken to unwrapAtlas.
    float maxChartDistortion = 0.10f;
    // Rotate each chart to its minimum-area bounding rectangle before packing.
    // LSCM fixes orientation from its pinned corners, so charts otherwise land
    // at arbitrary angles and waste space in the axis-aligned shelf packer.
    bool reorientCharts = true;
    UnwrapOptions unwrap{};
    PackParams pack{};
};

struct AtlasResult {
    // True only when every chart was parameterized (with finite UVs) and the
    // pack succeeded. A mesh carrying a non-finite vertex position reports
    // ok=false instead of writing non-finite UVs into the atlas.
    bool ok = false;
    // Set when the caller's CancelToken was tripped. The mesh is left exactly as
    // it was: cancellation is only observed before any UV is written, so a
    // cancelled run never leaves a half-written atlas behind. All other fields
    // are meaningless then.
    bool cancelled = false;
    // Charts that actually occupy area in the packed atlas.
    int chartCount = 0;
    // Charts the seams produced that cover nothing once packed: LSCM and the
    // planar fallback both came out degenerate (a zero-area or collinear
    // island), so they are invisible in the layout. chartCount + droppedCharts
    // is the number of islands the seam set cut the mesh into.
    int droppedCharts = 0;
    std::size_t seamEdges = 0;
    // Worst / RMS conformal (angle) error across all charts, in [0, 1).
    float maxAngleDistortion = 0.0f;
    float rmsAngleDistortion = 0.0f;
    // Charts whose UVs came from the planar-projection fallback because LSCM
    // reported a degenerate island.
    int fallbackCharts = 0;
    // Charts whose net UV winding is mirrored (non-disk or folded).
    int flippedCharts = 0;
    // Fraction of the unit square the chart GEOMETRY covers (summed UV face
    // areas) — the texel efficiency a painter sees.
    float packedArea = 0.0f;
    // Fraction covered by the charts' bounding boxes: how tightly the packer
    // placed them, regardless of how much of each box its chart fills. The gap
    // to packedArea is the slack inside the charts' own boxes; a folded chart
    // (see flippedCharts) can invert it by covering its own faces twice.
    float packedBoxArea = 0.0f;
    float texelDensity = 0.0f;
};

// Partitions the mesh into normal-coherent charts and returns the seam edges
// between them (mesh boundary edges are already island boundaries and are not
// marked). Exposed for interactive seam preview and for testing that
// computeIslands reproduces the same partition.
//
// `progress` reports the distortion merge pass (see AtlasOptions::
// maxChartDistortion — the only pass whose cost is unbounded). `cancel` is
// polled between that pass's trial unwraps; when it trips, the merges made so
// far are DISCARDED and the returned seams are the ones the unmerged charts
// imply, so a caller that cancels must check its own token rather than trust
// the result.
[[nodiscard]] SeamSet autoSeams(const Mesh& mesh, const AtlasOptions& options = {},
                                ProgressSink* progress = nullptr,
                                const CancelToken* cancel = nullptr);

// Full automatic atlas: autoSeams -> computeIslands -> LSCM per chart (with a
// planar-projection fallback for degenerate charts) -> pack -> write "uv".
// Returns aggregate distortion and packing statistics.
//
// The default options run the distortion merge pass, which is the expensive one
// (see AtlasOptions::maxChartDistortion); pass a `cancel` token so a host can
// abort it. Cancellation is observed before the mesh is written, so
// AtlasResult::cancelled comes with an untouched mesh. Cancel latency is one
// trial unwrap, not the whole pass.
AtlasResult unwrapAtlas(Mesh& mesh, const AtlasOptions& options = {},
                        ProgressSink* progress = nullptr, const CancelToken* cancel = nullptr);

}  // namespace cyber::uv
