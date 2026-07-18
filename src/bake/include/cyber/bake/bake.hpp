#pragma once

#include <cstddef>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

// High-to-low surface baking (surface-baking spec). Bakes detail from a Target
// (high-poly) onto the UV layout of an EditMesh (low-poly). Ray casting is
// dispatched through the compute-acceleration layer, so the CPU backend is the
// reference and a GPU backend accelerates it transparently (spec: "Accelerated,
// cancellable baking"). Cooperative cancellation leaves the caller free to keep
// the previous maps.
namespace cyber::bake {

enum class BakeMap {
    Normal,             // tangent-space normal map (RGB, encoded [0,1])
    AmbientOcclusion,   // openness in [0,1] (1 = fully lit), single channel
    Displacement,       // signed height along the low-poly normal, single channel
    Position,           // high-poly hit position (RGB, world space)
    Color,              // Target vertex color sampled at the hit (RGB)
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

struct BakeParams {
    int width = 512;
    int height = 512;
    // Projection cage: rays start at (surface + normal*cageDistance) and cast
    // inward up to 2*cageDistance to find the Target (11.2 makes this a
    // per-vertex editable cage; here it is a uniform distance).
    float cageDistance = 0.1f;
    int aoSamples = 16;       // hemisphere rays per texel for AO
    float aoRadius = 1.0f;    // an AO ray hit beyond this does not occlude
    float aoBias = 1e-3f;     // start offset to avoid self-hits
};

struct BakeResult {
    Image image;
    bool cancelled = false;
    std::size_t texelsCovered = 0;  // texels touched by the UV layout
};

// Bakes `map` from `highPoly` onto the per-corner "uv" layout of `lowPoly`.
// `lowPoly` must carry a Vec2 "uv" corner attribute; missing UVs or a
// degenerate request yield an empty (zero-size) image.
[[nodiscard]] BakeResult bake(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map,
                              const BakeParams& params, ProgressSink* progress = nullptr,
                              const CancelToken* cancel = nullptr);

}  // namespace cyber::bake
