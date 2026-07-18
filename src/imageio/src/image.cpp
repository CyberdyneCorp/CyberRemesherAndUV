#include "cyber/imageio/image.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <vector>

#include "cyber/imageio/exr.hpp"
#include "cyber/imageio/png.hpp"

namespace cyber::imageio {

namespace {

bool savePng(const std::string& path, const cyber::bake::Image& image) {
    if (image.width <= 0 || image.height <= 0) {
        return false;
    }
    if (image.channels == 3 || image.channels == 4) {
        return writePngTonemapped(path, image.width, image.height, image.channels,
                                  image.pixels.data());
    }
    if (image.channels == 1) {
        // Expand grayscale to RGB so the 8-bit PNG path (which needs 3/4
        // channels) can encode single-channel bake maps.
        const std::size_t count =
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
        std::vector<float> rgb(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            const float v = image.pixels[i];
            rgb[i * 3 + 0] = v;
            rgb[i * 3 + 1] = v;
            rgb[i * 3 + 2] = v;
        }
        return writePngTonemapped(path, image.width, image.height, 3, rgb.data());
    }
    return false;
}

bool hasExrExtension(const std::string& path) {
    if (path.size() < 4) {
        return false;
    }
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".exr";
}

}  // namespace

bool saveImage(const std::string& path, const cyber::bake::Image& image, ImageFormat format) {
    if (format == ImageFormat::Exr) {
        return writeExr(path, image.width, image.height, image.channels, image.pixels.data());
    }
    return savePng(path, image);
}

bool saveImage(const std::string& path, const cyber::bake::Image& image) {
    return saveImage(path, image, hasExrExtension(path) ? ImageFormat::Exr : ImageFormat::Png);
}

}  // namespace cyber::imageio
