#include <doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cyber/imageio/load.hpp"
#include "cyber/imageio/png.hpp"

namespace {

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void writeRawFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

}  // namespace

TEST_CASE("loadPng round-trips a 4x4 RGB image written by writePng") {
    constexpr int w = 4;
    constexpr int h = 4;
    constexpr int ch = 3;
    std::array<std::uint8_t, w * h * ch> src{};
    for (std::size_t i = 0; i < src.size(); ++i) {
        // Deterministic pattern spanning the 0..255 range.
        src[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xff);
    }

    const std::string path = tempPath("cyber_load_rgb.png");
    REQUIRE(cyber::imageio::writePng(path, w, h, ch, src.data()));

    const auto loaded = cyber::imageio::loadPng(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->width == w);
    CHECK(loaded->height == h);
    CHECK(loaded->channels == ch);
    REQUIRE(loaded->pixels.size() == src.size());

    const float tol = 1.0f / 255.0f;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const float expected = static_cast<float>(src[i]) / 255.0f;
        CHECK(std::fabs(loaded->pixels[i] - expected) <= tol);
    }
}

TEST_CASE("loadPng round-trips a 4x4 RGBA image written by writePng") {
    constexpr int w = 4;
    constexpr int h = 4;
    constexpr int ch = 4;
    std::array<std::uint8_t, w * h * ch> src{};
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<std::uint8_t>((i * 53 + 7) & 0xff);
    }

    const std::string path = tempPath("cyber_load_rgba.png");
    REQUIRE(cyber::imageio::writePng(path, w, h, ch, src.data()));

    const auto loaded = cyber::imageio::loadPng(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->channels == 4);
    REQUIRE(loaded->pixels.size() == src.size());

    const float tol = 1.0f / 255.0f;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const float expected = static_cast<float>(src[i]) / 255.0f;
        CHECK(std::fabs(loaded->pixels[i] - expected) <= tol);
    }
}

TEST_CASE("sampleBilinear at a texel centre returns that texel exactly") {
    constexpr int w = 4;
    constexpr int h = 4;
    constexpr int ch = 4;
    std::array<std::uint8_t, w * h * ch> src{};
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<std::uint8_t>((i * 17 + 3) & 0xff);
    }
    const std::string path = tempPath("cyber_load_sample.png");
    REQUIRE(cyber::imageio::writePng(path, w, h, ch, src.data()));
    const auto loaded = cyber::imageio::loadPng(path);
    REQUIRE(loaded.has_value());

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            const std::array<float, 4> s = cyber::imageio::sampleBilinear(*loaded, u, v);
            const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                      static_cast<std::size_t>(x)) *
                                     static_cast<std::size_t>(ch);
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(s[c] == doctest::Approx(loaded->pixels[base + c]));
            }
        }
    }
}

TEST_CASE("sampleBilinear clamps to edge and reports alpha 1 for RGB") {
    constexpr int w = 2;
    constexpr int h = 2;
    constexpr int ch = 3;
    const std::array<std::uint8_t, w * h * ch> src = {
        10, 20, 30, 40,  50,  60,  // row 0
        70, 80, 90, 100, 110, 120  // row 1
    };
    const std::string path = tempPath("cyber_load_clamp.png");
    REQUIRE(cyber::imageio::writePng(path, w, h, ch, src.data()));
    const auto loaded = cyber::imageio::loadPng(path);
    REQUIRE(loaded.has_value());

    // Far outside [0,1] clamps to the corner texel; RGB image reports alpha 1.
    const std::array<float, 4> topLeft = cyber::imageio::sampleBilinear(*loaded, -5.0f, -5.0f);
    CHECK(topLeft[0] == doctest::Approx(10.0f / 255.0f));
    CHECK(topLeft[1] == doctest::Approx(20.0f / 255.0f));
    CHECK(topLeft[2] == doctest::Approx(30.0f / 255.0f));
    CHECK(topLeft[3] == doctest::Approx(1.0f));

    const std::array<float, 4> bottomRight = cyber::imageio::sampleBilinear(*loaded, 5.0f, 5.0f);
    CHECK(bottomRight[0] == doctest::Approx(100.0f / 255.0f));
    CHECK(bottomRight[3] == doctest::Approx(1.0f));
}

TEST_CASE("loadPng rejects a truncated PNG") {
    constexpr int w = 4;
    constexpr int h = 4;
    std::array<std::uint8_t, w * h * 3> src{};
    src.fill(128);
    const std::string path = tempPath("cyber_load_full.png");
    REQUIRE(cyber::imageio::writePng(path, w, h, 3, src.data()));

    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 40);
    bytes.resize(bytes.size() / 2);  // chop the file in half
    const std::string truncated = tempPath("cyber_load_truncated.png");
    writeRawFile(truncated, bytes);

    CHECK_FALSE(cyber::imageio::loadPng(truncated).has_value());
}

TEST_CASE("loadPng rejects garbage and missing files") {
    const std::string garbagePath = tempPath("cyber_load_garbage.png");
    writeRawFile(garbagePath, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    CHECK_FALSE(cyber::imageio::loadPng(garbagePath).has_value());

    CHECK_FALSE(cyber::imageio::loadPng(tempPath("cyber_load_does_not_exist.png")).has_value());
}
