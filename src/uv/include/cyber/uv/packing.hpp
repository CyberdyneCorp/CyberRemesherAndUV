#pragma once

#include <span>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// Island packing (uv-editing spec, "Packing"): shelf packing of island
// bounding boxes into the 0-1 UV square with a uniform scale that preserves
// relative texel density, plus a texel-density readout. Designed to handle
// large island counts without failure.
namespace cyber::uv {

// Placement strategy for packBoxes.
enum class PackStrategy {
    // Tallest-first shelf packing: simple, stable, good for interactive edits.
    Shelf,
    // Bottom-left skyline over a fixed-width strip: drops each box into the
    // lowest gap, filling the vertical slack shelves leave. Tighter, at a
    // little more compute — the default for the automatic atlas.
    Skyline,
};

struct PackParams {
    // Gap left around every island, in the SAME units as the input boxes
    // (i.e. UV units before the final normalize).
    float margin = 0.0f;
    // Texture resolution used for the texel-density readout.
    int textureSize = 1024;
    // Placement strategy (see PackStrategy).
    PackStrategy strategy = PackStrategy::Shelf;
};

struct PackedIsland {
    // Transform mapping an original UV p to its packed position:
    //   packed = (p - source.mn) * scale + offset
    Vec2 offset{};
    float scale = 1.0f;
    Bounds2 source;  // original box (before packing)
    Bounds2 placed;  // final box inside the unit square
};

struct PackResult {
    bool ok = false;
    std::vector<PackedIsland> islands;  // one per input box, input order
    float scale = 1.0f;                 // uniform scale applied to every box
    float usedArea = 0.0f;              // fraction of the unit square covered
    float texelDensity = 0.0f;          // texels per UV unit at `scale`
};

// Column count of the skyline height map. Exposed because the two run-scanners below are
// defined over exactly this many columns.
constexpr int kSkylineColumns = 512;

// Lowest resting height over every run of `span` consecutive columns of `heights`, returning
// {leftmost column achieving it, that height}.
//
// TWO implementations of one function, exposed so a test can assert they agree. `packBoxes`
// picks between them by `span`, and which is faster is a measured question rather than an
// asymptotic one:
//
//  * Naive: ~kSkylineColumns * span. Cheaper when the strip is wide relative to the islands,
//    so each island spans only a column or two.
//  * Monotonic: a queue of column indices whose front is the tallest column in the current
//    run, ~kSkylineColumns plus bookkeeping. Wins by ~6x once span is large.
//
// Both use the same tie-break — ascending column, strictly-less comparison — so they are
// interchangeable and the packed output does not depend on which one runs.
[[nodiscard]] std::pair<int, float> lowestRunNaive(std::span<const float> heights, int span);
[[nodiscard]] std::pair<int, float> lowestRunMonotonic(std::span<const float> heights, int span,
                                                       std::vector<int>& scratch);

// Packs `boxes` (island bounding boxes) into the unit square with a single
// uniform scale (relative sizes preserved). Boxes may be empty; the result is
// overlap-free at the requested margin.
[[nodiscard]] PackResult packBoxes(std::span<const Bounds2> boxes, const PackParams& params = {});

// Convenience: computes each island's UV bounds, packs them, and rewrites the
// per-loop "uv" attribute in place. Returns the packing result.
PackResult packIslands(Mesh& mesh, std::span<const std::vector<FaceId>> islands,
                       const PackParams& params = {});

// Texel density of one island: texels (at `textureSize`) per unit of 3D
// surface length, averaged over the island's edges. Zero when the island has
// no UVs or no measurable surface length.
[[nodiscard]] float texelDensity(const Mesh& mesh, std::span<const FaceId> island, int textureSize);

}  // namespace cyber::uv
