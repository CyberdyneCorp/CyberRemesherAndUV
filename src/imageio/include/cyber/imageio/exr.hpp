#pragma once

#include <string>

// Minimal, dependency-free OpenEXR writer (roadmap 3.4): scanline based,
// NO_COMPRESSION, 32-bit float channels. Handles 1 (Y), 3 (RGB) and 4 (RGBA)
// channel images. The output follows the OpenEXR file layout — magic, version,
// header attributes, scanline offset table, then uncompressed scanline blocks —
// and is readable by any conformant EXR reader.
namespace cyber::imageio {

// Writes an OpenEXR image from float pixels. `channels` must be 1, 3 or 4;
// `pixels` is row-major, width*height*channels floats, in R,G,B,A order. Returns
// false on invalid arguments or I/O failure.
[[nodiscard]] bool writeExr(const std::string& path, int width, int height, int channels,
                            const float* pixels);

}  // namespace cyber::imageio
