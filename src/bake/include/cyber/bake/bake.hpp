#pragma once

#include <cstddef>
#include <vector>

#include "cyber/bake/field_evaluator.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/imageio/load.hpp"

// High-to-low surface baking (surface-baking spec). Bakes detail from a Target
// (high-poly) onto the UV layout of an EditMesh (low-poly). Ray casting is
// dispatched through the compute-acceleration layer, so the CPU backend is the
// reference and a GPU backend accelerates it transparently (spec: "Accelerated,
// cancellable baking"). Cooperative cancellation leaves the caller free to keep
// the previous maps.
namespace cyber::bake {

enum class BakeMap {
    Normal,            // tangent-space normal map (RGB, encoded [0,1])
    AmbientOcclusion,  // openness in [0,1] (1 = fully lit), single channel
    Displacement,      // signed height along the low-poly normal, single channel
    Position,          // high-poly hit position (RGB, world space)
    Color,             // Target vertex color sampled at the hit (RGB)
    Curvature,         // signed mean curvature around mid-gray, single channel
    Cavity,            // concavity only (white = flat or convex), single channel
};

struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> pixels;  // row-major, size = width*height*channels

    [[nodiscard]] float& at(int x, int y, int c) {
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)) *
                          static_cast<std::size_t>(channels) +
                      static_cast<std::size_t>(c)];
    }
    [[nodiscard]] float at(int x, int y, int c) const {
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)) *
                          static_cast<std::size_t>(channels) +
                      static_cast<std::size_t>(c)];
    }
};

// Selects where BakeMap::Color reads the Target's color from. VertexColors
// (the default) samples the Target's per-vertex "color" attribute; Texture
// samples `texture` at the Target UV interpolated from the hit face's per-corner
// "uv" attribute. Texture falls back to vertex colors when `texture` is null or
// the Target carries no UVs, so existing bakes are unchanged (roadmap 11.1).
struct ColorSource {
    enum Kind { VertexColors, Texture };
    Kind kind = VertexColors;
    const cyber::imageio::LoadedImage* texture = nullptr;
};

// Numeric members are range-checked by bake() against the ranges named below,
// but only for the maps that read them: a value out of range is a degenerate
// request and yields an empty image rather than a map of NaN.
struct BakeParams {
    int width = 512;   // > 0
    int height = 512;  // > 0
    // Projection cage: rays start at (surface + normal*cageDistance) and cast
    // inward up to 2*cageDistance to find the Target (11.2 makes this a
    // per-vertex editable cage; here it is a uniform distance). Finite, >= 0.
    float cageDistance = 0.1f;
    // Hemisphere rays per texel for AO. Binary visibility quantizes openness to
    // aoSamples+1 rungs, so a low budget ships a visibly stepped map even with
    // the per-texel sample rotation dithering it; 64 is the smallest default
    // that reads as continuous in 8-bit. > 0 on the raycast path, which divides
    // the occluded-ray count by it.
    int aoSamples = 64;
    float aoRadius = 1.0f;    // an AO ray hit beyond this does not occlude; finite, >= 0
    float aoBias = 1e-3f;     // start offset to avoid self-hits; finite
    ColorSource colorSource;  // BakeMap::Color source (default: Target vertex colors)
    // Curvature magnitude (units 1/length) that saturates a Curvature/Cavity
    // bake to full white/black. 0 = auto: the 95th percentile of |curvature|
    // over the Target, which is scale-independent and keeps one pinched vertex
    // from flattening the map to mid-gray. Finite (a negative value is auto too).
    float curvatureRange = 0.0f;
    // Optional field evaluator (pipeline-bridge spec, "Field-sampled baking").
    // When set, Normal / AmbientOcclusion / Curvature / Cavity sample the field
    // directly — the cage ray is sphere-traced through it and normals come from
    // exact gradients instead of interpolated mesh normals. Every other map,
    // and every bake with `field == nullptr`, takes the raycast path with
    // BIT-IDENTICAL output. `highPoly` may be empty only when an evaluator is
    // attached and the requested map is one of the four it covers.
    const FieldEvaluator* field = nullptr;
};

struct BakeResult {
    Image image;
    bool cancelled = false;
    std::size_t texelsCovered = 0;  // texels touched by the UV layout
};

// Bakes `map` from `highPoly` onto the per-corner "uv" layout of `lowPoly`.
// `lowPoly` must carry a Vec2 "uv" corner attribute; missing UVs or a
// degenerate request — including a BakeParams member outside the range
// documented above — yield an empty (zero-size) image.
[[nodiscard]] BakeResult bake(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map,
                              const BakeParams& params, ProgressSink* progress = nullptr,
                              const CancelToken* cancel = nullptr);

}  // namespace cyber::bake
