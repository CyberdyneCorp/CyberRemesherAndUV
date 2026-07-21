#pragma once

#include <span>
#include <vector>

#include "cyber/core/mesh.hpp"
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
    UnwrapOptions unwrap{};
    PackParams pack{};
};

struct AtlasResult {
    bool ok = false;
    int chartCount = 0;
    std::size_t seamEdges = 0;
    // Worst / RMS conformal (angle) error across all charts, in [0, 1).
    float maxAngleDistortion = 0.0f;
    float rmsAngleDistortion = 0.0f;
    // Charts whose UVs came from the planar-projection fallback because LSCM
    // reported a degenerate island.
    int fallbackCharts = 0;
    // Charts whose net UV winding is mirrored (non-disk or folded).
    int flippedCharts = 0;
    // Packing outcome (fraction of the unit square covered, texel density).
    float packedArea = 0.0f;
    float texelDensity = 0.0f;
};

// Partitions the mesh into normal-coherent charts and returns the seam edges
// between them (mesh boundary edges are already island boundaries and are not
// marked). Exposed for interactive seam preview and for testing that
// computeIslands reproduces the same partition.
[[nodiscard]] SeamSet autoSeams(const Mesh& mesh, const AtlasOptions& options = {});

// Full automatic atlas: autoSeams -> computeIslands -> LSCM per chart (with a
// planar-projection fallback for degenerate charts) -> pack -> write "uv".
// Returns aggregate distortion and packing statistics.
AtlasResult unwrapAtlas(Mesh& mesh, const AtlasOptions& options = {});

}  // namespace cyber::uv
