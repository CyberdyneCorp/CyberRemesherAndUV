#pragma once

#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// Island packing (uv-editing spec, "Packing"): shelf packing of island
// bounding boxes into the 0-1 UV square with a uniform scale that preserves
// relative texel density, plus a texel-density readout. Designed to handle
// large island counts without failure.
namespace cyber::uv {

struct PackParams {
    // Gap left around every island, in the SAME units as the input boxes
    // (i.e. UV units before the final normalize).
    float margin = 0.0f;
    // Texture resolution used for the texel-density readout.
    int textureSize = 1024;
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
[[nodiscard]] float texelDensity(const Mesh& mesh, std::span<const FaceId> island,
                                 int textureSize);

}  // namespace cyber::uv
