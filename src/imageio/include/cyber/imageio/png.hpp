#pragma once

#include <cstdint>
#include <string>

// Minimal, dependency-free PNG writer (roadmap 3.4). Emits 8-bit RGB/RGBA
// images using a zlib stream of STORED (uncompressed) deflate blocks with a
// correct Adler-32 trailer and per-chunk CRC-32. Output is a valid PNG readable
// by any conformant decoder; it trades file size for having zero third-party
// code in the map-export path.
namespace cyber::imageio {

// Writes an 8-bit PNG. `channels` must be 3 (RGB) or 4 (RGBA); `rgba` is
// row-major, width*height*channels bytes. Returns false on invalid arguments or
// I/O failure.
[[nodiscard]] bool writePng(const std::string& path, int width, int height, int channels,
                            const std::uint8_t* rgba);

// Writes an 8-bit PNG from float pixels, clamping [0,1] -> [0,255]. `channels`
// must be 3 or 4; `pixels` is row-major, width*height*channels floats.
[[nodiscard]] bool writePngTonemapped(const std::string& path, int width, int height, int channels,
                                      const float* pixels);

}  // namespace cyber::imageio
