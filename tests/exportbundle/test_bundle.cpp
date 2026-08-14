#include <doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/exportbundle/bundle.hpp"
#include "cyber/imageio/load.hpp"

namespace fs = std::filesystem;
namespace io = cyber::io;
namespace bundle = cyber::exportbundle;

using cyber::CancelToken;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;

namespace {

fs::path testDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("cyber_bundle_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// A tilted quad pair: enough surface to rasterize into, tilted so the baked
// normal map is not uniformly flat and the green flip is observable.
Mesh makeSurface(float z) {
    const std::vector<Vec3> p = {{0, 0, z}, {1, 0, z}, {1, 1, z + 0.05f}, {0, 1, z + 0.05f}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(p, faces);
}

io::ExportPreset smallPreset(const std::string& name, io::GreenChannel green) {
    io::ExportPreset preset;
    preset.name = name;
    preset.meshFormat = "obj";
    preset.textureFormat = "png";
    preset.resolution = 32;
    preset.normalGreen = green;
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "normal"},
        io::PresetMapEntry{io::PresetMap::Curvature, io::ColorSpace::Linear, "curvature"},
    };
    return preset;
}

bundle::BundleParams paramsFor(const io::ExportPreset& preset, const fs::path& dir) {
    bundle::BundleParams params;
    params.preset = preset;
    params.meshPath = dir / "hero.obj";
    params.cageDistance = 0.2f;
    return params;
}

std::vector<std::string> kindsOf(const bundle::BundleResult& result) {
    std::vector<std::string> kinds;
    for (const bundle::BundleFile& file : result.files) {
        kinds.push_back(file.kind);
    }
    return kinds;
}

// Green channel (0..255) of the first covered texel of a written normal map.
int firstGreen(const fs::path& path) {
    const auto loaded = cyber::imageio::loadPng(path.string());
    REQUIRE(loaded.has_value());
    const int ch = loaded->channels;
    const std::size_t texels =
        static_cast<std::size_t>(loaded->width) * static_cast<std::size_t>(loaded->height);
    for (std::size_t i = 0; i < texels; ++i) {
        const float blue = loaded->pixels[i * static_cast<std::size_t>(ch) + 2];
        if (blue > 0.5f) {  // blue high => a real tangent-space normal, not padding
            return static_cast<int>(loaded->pixels[i * static_cast<std::size_t>(ch) + 1] * 255.0f +
                                    0.5f);
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("a bundle writes the mesh plus exactly the preset's map set") {
    const fs::path dir = testDir("set");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    const bundle::BundleResult result =
        bundle::writeBundle(low, high, paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir));
    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(kindsOf(result) == std::vector<std::string>{"mesh", "normal", "curvature"});
    REQUIRE(fs::exists(dir / "hero.obj"));
    REQUIRE(fs::exists(dir / "hero_normal.png"));
    REQUIRE(fs::exists(dir / "hero_curvature.png"));
    // Nothing outside the declared bundle (the .mtl is the OBJ writer's own
    // sibling, so count only what the preset named).
    REQUIRE_FALSE(fs::exists(dir / "hero_ao.png"));
    fs::remove_all(dir);
}

TEST_CASE("a UV-less low-poly is unwrapped rather than refused") {
    const fs::path dir = testDir("unwrap");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    REQUIRE(low.cornerAttributes().find<cyber::Vec2>(io::kUvAttribute) == nullptr);

    const bundle::BundleResult result =
        bundle::writeBundle(low, high, paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir));
    REQUIRE(result.ok);
    REQUIRE(result.unwrapped);
    REQUIRE(result.chartCount > 0);
    // The UVs are written back to the caller's mesh, so the exported OBJ and
    // the maps agree on one layout.
    REQUIRE(low.cornerAttributes().find<cyber::Vec2>(io::kUvAttribute) != nullptr);
    fs::remove_all(dir);
}

TEST_CASE("the DirectX preset flips the normal map's green channel") {
    const fs::path glDir = testDir("green_gl");
    const fs::path dxDir = testDir("green_dx");
    Mesh lowGl = makeSurface(0.0f);
    Mesh lowDx = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    REQUIRE(bundle::writeBundle(lowGl, high,
                                paramsFor(smallPreset("gl", io::GreenChannel::PlusY), glDir))
                .ok);
    REQUIRE(bundle::writeBundle(lowDx, high,
                                paramsFor(smallPreset("dx", io::GreenChannel::MinusY), dxDir))
                .ok);

    const int gl = firstGreen(glDir / "hero_normal.png");
    const int dx = firstGreen(dxDir / "hero_normal.png");
    REQUIRE(gl >= 0);
    REQUIRE(dx >= 0);
    // The flip is exactly 1 - g, so the two must sum to full scale.
    REQUIRE(gl + dx == doctest::Approx(255).epsilon(0.01));
    fs::remove_all(glDir);
    fs::remove_all(dxDir);
}

TEST_CASE("an sRGB map is encoded, and the report says which space was written") {
    const fs::path dir = testDir("srgb");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.maps = {io::PresetMapEntry{io::PresetMap::Color, io::ColorSpace::Srgb, "color"}};
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE(result.ok);
    REQUIRE(result.files.size() == 2);
    REQUIRE(result.files[1].kind == "color");
    REQUIRE(result.files[1].colorSpace == "srgb");
    REQUIRE(result.files[1].width == 32);
    fs::remove_all(dir);
}

TEST_CASE("sRGB on an EXR preset warns instead of silently encoding") {
    const fs::path dir = testDir("srgb_exr");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.textureFormat = "exr";
    preset.maps = {io::PresetMapEntry{io::PresetMap::Color, io::ColorSpace::Srgb, "color"}};
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE(result.ok);
    // EXR is linear float by convention. The preset's request is neither
    // honored silently nor dropped silently: it is reported and recorded.
    REQUIRE(result.files[1].colorSpace == "linear");
    REQUIRE(result.warnings.size() == 1);
    REQUIRE(result.warnings[0].find("EXR") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("a mesh-format mismatch warns but honors the explicit output path") {
    const fs::path dir = testDir("format");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.meshFormat = "glb";  // the path below says .obj
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE(result.ok);
    REQUIRE(fs::exists(dir / "hero.obj"));
    REQUIRE(result.warnings.size() == 1);
    REQUIRE(result.warnings[0].find("glb") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("a naming pattern that escapes the output directory is refused") {
    // Regression: the joined path was written with no containment check, so an
    // absolute pattern (or one climbing with "..") from a third-party preset
    // file wrote every map outside the caller's chosen directory — and the
    // bundle reported ok, listing the escaped paths as part of it. parsePreset
    // rejects such a pattern now; this pins the writer, which also sees presets
    // built in C++ that never went through the parser.
    const fs::path dir = testDir("escape");
    const fs::path outside = fs::temp_directory_path() / "cyber_bundle_escapee_normal.png";
    fs::remove(outside);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.maps = {io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "normal"}};
    preset.namingPattern =
        (fs::temp_directory_path() / "cyber_bundle_escapee_{map}.{ext}").string();

    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE_FALSE(fs::exists(outside));
    // The escaped path must not be reported as part of the bundle either.
    REQUIRE(kindsOf(result) == std::vector<std::string>{"mesh"});

    // "..' climbs out just as effectively and must fail the same way.
    preset.namingPattern = "../cyber_bundle_escapee_{map}.{ext}";
    Mesh low2 = makeSurface(0.0f);
    REQUIRE_FALSE(bundle::writeBundle(low2, high, paramsFor(preset, dir)).ok);
    REQUIRE_FALSE(fs::exists(dir.parent_path() / "cyber_bundle_escapee_normal.png"));
    fs::remove_all(dir);
}

TEST_CASE("the container follows the preset's textureFormat, not the file name") {
    // Regression: the map was handed to the extension-dispatching saveImage(),
    // so a pattern without (or contradicting) {ext} silently picked a different
    // container than the color-space policy just applied assumed — 8-bit
    // tonemapped PNG data under an "exr" preset, or gamma-encoded values inside
    // a float EXR under a "png" one.
    const fs::path dir = testDir("container");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.textureFormat = "exr";
    preset.namingPattern = "{basename}_{map}";  // no extension to dispatch on
    preset.maps = {io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "normal"}};

    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE(result.ok);
    const fs::path written = dir / "hero_normal";
    REQUIRE(fs::exists(written));

    std::ifstream file(written, std::ios::binary);
    REQUIRE(file.good());
    std::array<unsigned char, 4> magic{};
    file.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    // OpenEXR's magic (0x01312f76, little-endian) — a PNG would start 0x89 P N G.
    REQUIRE(magic == std::array<unsigned char, 4>{0x76, 0x2f, 0x31, 0x01});
    fs::remove_all(dir);
}

TEST_CASE("bundling honors cooperative cancellation") {
    const fs::path dir = testDir("cancel");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    CancelToken cancel;
    cancel.requestCancel();

    const bundle::BundleResult result = bundle::writeBundle(
        low, high, paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir), nullptr, &cancel);
    REQUIRE(result.cancelled);
    REQUIRE_FALSE(result.ok);
    fs::remove_all(dir);
}
