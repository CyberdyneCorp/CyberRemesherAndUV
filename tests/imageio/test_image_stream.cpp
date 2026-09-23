#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/imageio/image.hpp"
#include "cyber/imageio/stream.hpp"

// Band-streamed map output (mesh-io spec, "Baked maps are writable one band at a
// time"). The band writer is a SEPARATE encoder from the one-shot saveImage();
// what makes it trustworthy is that its bytes equal the one-shot bytes for every
// band size, which is what every case here asserts.
namespace {

using Bytes = std::vector<std::uint8_t>;
namespace imageio = cyber::imageio;

std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

Bytes readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A map whose values run outside [0,1] (so the PNG clamp is exercised) and
// differ in every channel.
cyber::bake::Image pattern(int width, int height, int channels) {
    cyber::bake::Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.resize(static_cast<std::size_t>(width * height * channels));
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        image.pixels[i] = std::sin(static_cast<float>(i) * 0.37f) * 0.7f + 0.45f;
    }
    return image;
}

Bytes streamed(const cyber::bake::Image& image, imageio::ImageFormat format, int band,
               const std::string& path) {
    auto writer =
        imageio::ImageStreamWriter::open(path, image.width, image.height, image.channels, format);
    REQUIRE(writer != nullptr);
    const std::size_t stride =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.channels);
    for (int y = 0; y < image.height; y += band) {
        const int rows = std::min(band, image.height - y);
        REQUIRE(
            writer->writeRows(image.pixels.data() + static_cast<std::size_t>(y) * stride, rows));
    }
    REQUIRE(writer->finish());
    return readFile(path);
}

}  // namespace

TEST_CASE("band-streamed PNG and EXR files equal the one-shot files") {
    const std::string oneShot = tempPath("cyber_stream_oneshot.bin");
    const std::string banded = tempPath("cyber_stream_banded.bin");
    // 131 wide x 3 channels makes a row of 394 raw bytes, so the 65535-byte
    // deflate blocks fall mid-row and across bands; 173 x 4 x 95 rows runs the
    // raw stream past one block and 300 x 3 x 200 past several.
    for (const int channels : {1, 3, 4}) {
        for (const auto& size : {std::array<int, 2>{131, 97}, std::array<int, 2>{173, 95},
                                 std::array<int, 2>{300, 200}}) {
            const cyber::bake::Image image = pattern(size[0], size[1], channels);
            for (const imageio::ImageFormat format :
                 {imageio::ImageFormat::Png, imageio::ImageFormat::Exr}) {
                REQUIRE(imageio::saveImage(oneShot, image, format));
                const Bytes expected = readFile(oneShot);
                for (const int band : {1, 7, 32, 1000}) {
                    CAPTURE(channels);
                    CAPTURE(band);
                    CAPTURE(static_cast<int>(format));
                    CHECK(streamed(image, format, band, banded) == expected);
                }
            }
        }
    }
    std::filesystem::remove(oneShot);
    std::filesystem::remove(banded);
}

TEST_CASE("a raw stream that is an exact multiple of the deflate block keeps the one-shot split") {
    // RGBA rows of 64 texels are 257 raw bytes with the filter byte, and
    // 257 * 255 == 65535: exactly one full block, which must be the FINAL one.
    const cyber::bake::Image image = pattern(64, 255, 4);
    const std::string oneShot = tempPath("cyber_stream_exact_oneshot.png");
    const std::string banded = tempPath("cyber_stream_exact_banded.png");
    REQUIRE(imageio::saveImage(oneShot, image, imageio::ImageFormat::Png));
    for (const int band : {1, 5, 255}) {
        CHECK(streamed(image, imageio::ImageFormat::Png, band, banded) == readFile(oneShot));
    }
    std::filesystem::remove(oneShot);
    std::filesystem::remove(banded);
}

TEST_CASE("a band writer refuses too many rows, too few rows and unusable arguments") {
    const std::string path = tempPath("cyber_stream_refusals.bin");
    std::filesystem::remove(path);
    CHECK(imageio::ImageStreamWriter::open(path, 0, 4, 3, imageio::ImageFormat::Png) == nullptr);
    CHECK(imageio::ImageStreamWriter::open(path, 4, 4, 2, imageio::ImageFormat::Exr) == nullptr);
    // An unusable request leaves no file behind.
    CHECK_FALSE(std::filesystem::exists(path));

    for (const imageio::ImageFormat format :
         {imageio::ImageFormat::Png, imageio::ImageFormat::Exr}) {
        const cyber::bake::Image image = pattern(8, 4, 3);
        auto tooMany = imageio::ImageStreamWriter::open(path, 8, 4, 3, format);
        REQUIRE(tooMany != nullptr);
        CHECK(tooMany->writeRows(image.pixels.data(), 3));
        CHECK_FALSE(tooMany->writeRows(image.pixels.data(), 2));  // runs past the height

        auto tooFew = imageio::ImageStreamWriter::open(path, 8, 4, 3, format);
        REQUIRE(tooFew != nullptr);
        CHECK(tooFew->writeRows(image.pixels.data(), 3));
        CHECK_FALSE(tooFew->finish());  // one row short
    }
    std::filesystem::remove(path);
}

TEST_CASE("a band writer to an unwritable path is refused at open") {
    CHECK(imageio::ImageStreamWriter::open("/nonexistent-cyber-dir/map.png", 4, 4, 3,
                                           imageio::ImageFormat::Png) == nullptr);
}
