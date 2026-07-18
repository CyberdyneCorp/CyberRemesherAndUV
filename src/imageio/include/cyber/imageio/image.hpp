#pragma once

#include <string>

#include "cyber/bake/bake.hpp"

// Convenience layer bridging baked maps (cyber::bake::Image, float pixels,
// row-major) to the raw encoders (roadmap 3.4). PNG output is tonemapped
// (clamped [0,1] -> 8-bit); EXR preserves the full float range. Single-channel
// bake maps (AO/displacement) are expanded to grayscale RGB for PNG.
namespace cyber::imageio {

enum class ImageFormat { Png, Exr };

// Saves a baked image in the requested format. Returns false on unsupported
// channel counts or I/O failure.
[[nodiscard]] bool saveImage(const std::string& path, const cyber::bake::Image& image,
                             ImageFormat format);

// Saves a baked image, choosing the format from the path extension (".exr" ->
// EXR, everything else -> PNG).
[[nodiscard]] bool saveImage(const std::string& path, const cyber::bake::Image& image);

}  // namespace cyber::imageio
