#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Dependency-free general 8-bit PNG reader complementing the writer in png.hpp
// (roadmap 3.4 / 11.1). It decodes real-world PNGs (e.g. written by PIL /
// matplotlib): a full RFC 1951 inflate (stored + fixed- and dynamic-Huffman
// blocks with LZ77 back-references), all five scanline filters (None, Sub, Up,
// Average, Paeth), and colour types 0 (grayscale), 2 (RGB), 6 (RGBA) and 3
// (palette via PLTE, honouring tRNS). Grayscale and palette expand to RGB(A);
// output has 3 or 4 channels. The zlib header, Adler-32 trailer and per-chunk
// CRC-32 are all validated. 16-bit depth and interlaced PNGs are rejected
// cleanly (loadPng returns std::nullopt), as is any corrupt stream. The
// writer's own STORED-DEFLATE / filter-None output still round-trips.
namespace cyber::imageio {

// Decoded image: row-major pixels, one float per channel, normalised to [0,1].
// channels is 3 (RGB) or 4 (RGBA). pixels.size() == width*height*channels.
struct LoadedImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> pixels;
};

// Loads an 8-bit RGB/RGBA PNG produced by writePng. Returns std::nullopt on a
// missing/truncated/corrupt file, a failed checksum, or any feature outside the
// writer's STORED-DEFLATE, filter-None output (see the file comment above).
[[nodiscard]] std::optional<LoadedImage> loadPng(const std::string& path);

// Bilinearly samples the image at texture coordinates (u,v), clamping to the
// edge outside [0,1]. Texel centres sit at ((x+0.5)/w, (y+0.5)/h), so sampling a
// centre returns that texel exactly. Returns RGBA; alpha is 1 when the image has
// fewer than 4 channels. An empty image yields opaque black.
//
// Header-inline so consumers (e.g. the baker's texture color source) need only
// this header, not a link to cyber_imageio — which keeps bake and imageio from
// forming a static-library dependency cycle.
[[nodiscard]] inline std::array<float, 4> sampleBilinear(const LoadedImage& image, float u,
                                                         float v) {
    if (image.width <= 0 || image.height <= 0 || image.channels < 3) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const int w = image.width;
    const int h = image.height;
    const int ch = image.channels;
    const float fx = u * static_cast<float>(w) - 0.5f;
    const float fy = v * static_cast<float>(h) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto clampCoord = [](int c, int limit) { return std::min(std::max(c, 0), limit - 1); };
    const int xa = clampCoord(x0, w);
    const int xb = clampCoord(x0 + 1, w);
    const int ya = clampCoord(y0, h);
    const int yb = clampCoord(y0 + 1, h);
    const auto texel = [&](int x, int y) {
        const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                  static_cast<std::size_t>(x)) *
                                 static_cast<std::size_t>(ch);
        std::array<float, 4> t = {0.0f, 0.0f, 0.0f, 1.0f};
        t[0] = image.pixels[base];
        t[1] = image.pixels[base + 1];
        t[2] = image.pixels[base + 2];
        if (ch == 4) {
            t[3] = image.pixels[base + 3];
        }
        return t;
    };
    const std::array<float, 4> t00 = texel(xa, ya);
    const std::array<float, 4> t10 = texel(xb, ya);
    const std::array<float, 4> t01 = texel(xa, yb);
    const std::array<float, 4> t11 = texel(xb, yb);
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    std::array<float, 4> out{};
    for (std::size_t c = 0; c < 4; ++c) {
        out[c] = mix(mix(t00[c], t10[c], tx), mix(t01[c], t11[c], tx), ty);
    }
    return out;
}

}  // namespace cyber::imageio
