#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
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

TEST_CASE("a preset name or basename that escapes through a token is refused") {
    // The parse-time gate ran on the RAW namingPattern, so an escape could
    // arrive through a token it expands instead — the preset's own name via
    // {preset}, or the caller's basename. Expansion now refuses such a name,
    // and the writer must report that refusal rather than join an empty name
    // to the output directory (which would "write" the directory itself).
    const fs::path dir = testDir("token_escape");
    const fs::path outside = dir.parent_path() / "cyber_bundle_token_escapee_normal.png";
    fs::remove(outside);

    io::ExportPreset preset = smallPreset("../cyber_bundle_token_escapee", io::GreenChannel::PlusY);
    preset.maps = {io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "normal"}};
    preset.namingPattern = "{preset}_{map}.{ext}";

    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE_FALSE(fs::exists(outside));
    REQUIRE(kindsOf(result) == std::vector<std::string>{"mesh"});

    // The caller-supplied basename is the other token that reaches the path.
    preset.name = "t";
    preset.namingPattern = "{basename}_{map}.{ext}";
    bundle::BundleParams params = paramsFor(preset, dir);
    params.basename = "../cyber_bundle_token_escapee";
    Mesh low2 = makeSurface(0.0f);
    REQUIRE_FALSE(bundle::writeBundle(low2, high, params).ok);
    REQUIRE_FALSE(fs::exists(outside));
    fs::remove_all(dir);
}

TEST_CASE("two maps sharing a file name are refused instead of silently overwritten") {
    // Regression: the writer recomputed the path per entry with no memory of
    // what it had already written, so two entries expanding to one name left a
    // single file holding the LAST bake while result.files listed that path
    // twice, under two different kinds. A consumer reading the report then
    // binds the wrong map. parsePreset rejects such a preset file now; this
    // pins the writer, which also sees presets built in C++.
    const fs::path dir = testDir("collision");
    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "n"},
        io::PresetMapEntry{io::PresetMap::Curvature, io::ColorSpace::Linear, "n"},
    };

    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("hero_n.png") != std::string::npos);
    // The report carries the mesh and the ONE map actually written, never two
    // rows claiming one path.
    REQUIRE(kindsOf(result) == std::vector<std::string>{"mesh", "normal"});
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

    std::array<unsigned char, 4> magic{};
    {
        // Scoped so the handle is shut before remove_all: Windows refuses to
        // delete a file that is still open, where POSIX happily unlinks it.
        std::ifstream file(written, std::ios::binary);
        REQUIRE(file.good());
        file.read(reinterpret_cast<char*>(magic.data()),
                  static_cast<std::streamsize>(magic.size()));
    }
    // OpenEXR's magic (0x01312f76, little-endian) — a PNG would start 0x89 P N G.
    REQUIRE(magic == std::array<unsigned char, 4>{0x76, 0x2f, 0x31, 0x01});
    fs::remove_all(dir);
}

// The bundle used to hand the bake a null sink and report one step per map, so
// a preset whose slowest entry is a ray-traced map looked hung for the whole of
// it however finely the bake itself reported.
TEST_CASE("a bundle's progress moves WITHIN a map, not only between maps") {
    const fs::path dir = testDir("progress");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    // One ray-traced map, so every report between 0 and 1/1 comes from inside it.
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::BentNormal, io::ColorSpace::Linear, "bent"},
    };

    std::vector<float> values;
    float last = 0.0f;
    cyber::ProgressSink sink([&](float value, std::string_view) {
        CHECK(value >= last);
        last = value;
        values.push_back(value);
    });
    const bundle::BundleResult result =
        bundle::writeBundle(low, high, paramsFor(preset, dir), &sink);
    REQUIRE(result.ok);
    CHECK(std::count_if(values.begin(), values.end(),
                        [](float v) { return v > 0.0f && v < 1.0f; }) > 5);
    CHECK(last == doctest::Approx(1.0f));
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

// Regression: the token was consulted only in the per-map loop, so the unwrap a
// UV-less low-poly triggers — routinely the longest phase of the whole call, and
// unbounded on a large mesh — ran to completion first and wrote both the UVs and
// the mesh file before the cancel was noticed.
TEST_CASE("cancellation reaches the unwrap, not just the bakes") {
    const fs::path dir = testDir("cancel_unwrap");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);
    REQUIRE(low.cornerAttributes().find<cyber::Vec2>(io::kUvAttribute) == nullptr);
    CancelToken cancel;
    cancel.requestCancel();

    const bundle::BundleResult result = bundle::writeBundle(
        low, high, paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir), nullptr, &cancel);
    REQUIRE(result.cancelled);
    REQUIRE_FALSE(result.unwrapped);
    // Nothing was unwrapped and nothing was written: not the UVs on the
    // caller's mesh, not the mesh file the unwrap used to precede.
    REQUIRE(low.cornerAttributes().find<cyber::Vec2>(io::kUvAttribute) == nullptr);
    REQUIRE(result.files.empty());
    REQUIRE_FALSE(fs::exists(dir / "hero.obj"));
    fs::remove_all(dir);
}

// ---- the CyberTexel mesh-map set -------------------------------------------

TEST_CASE("a bundle writes the object-space and ray-traced maps with their basis") {
    const fs::path dir = testDir("mesh_maps");
    io::ExportPreset preset = smallPreset("mesh-maps", io::GreenChannel::PlusY);
    preset.upAxis = "z-up";  // a preset declares what its target app expects
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::ObjectNormal, io::ColorSpace::Linear, "object-normal"},
        io::PresetMapEntry{io::PresetMap::ObjectPosition, io::ColorSpace::Linear,
                           "object-position"},
        io::PresetMapEntry{io::PresetMap::BentNormal, io::ColorSpace::Linear, "bent-normal"},
        io::PresetMapEntry{io::PresetMap::Thickness, io::ColorSpace::Linear, "thickness"},
    };

    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.0f);
    bundle::BundleParams params = paramsFor(preset, dir);
    params.thicknessScale = 3.0f;
    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    REQUIRE(result.ok);
    CHECK(kindsOf(result) == std::vector<std::string>{"mesh", "object-normal", "object-position",
                                                      "bent-normal", "thickness"});

    const auto fileOf = [&](const std::string& kind) {
        for (const bundle::BundleFile& file : result.files) {
            if (file.kind == kind) {
                return file;
            }
        }
        FAIL("missing map " << kind);
        return bundle::BundleFile{};
    };
    // The mesh carries no encoding; every map carries the one its bake reported.
    CHECK(fileOf("mesh").encoding.basis == cyber::bake::EncodingBasis::None);
    CHECK(fileOf("object-normal").encoding.basis == cyber::bake::EncodingBasis::ObjectNormal);
    CHECK(fileOf("bent-normal").encoding.basis == cyber::bake::EncodingBasis::TangentNormal);

    const bundle::BundleFile position = fileOf("object-position");
    CHECK(position.encoding.basis == cyber::bake::EncodingBasis::ObjectBounds);
    // The preset's up axis is what the object-space maps were baked in, and the
    // recorded bounds are in that same convention.
    CHECK(position.encoding.upAxis == cyber::bake::UpAxis::ZUp);
    CHECK(position.encoding.boundsMax.z == doctest::Approx(1.0f));

    const bundle::BundleFile thickness = fileOf("thickness");
    CHECK(thickness.encoding.basis == cyber::bake::EncodingBasis::Distance);
    CHECK(thickness.encoding.scale == doctest::Approx(3.0f));

    // The padding record travels with the file too. The mesh entry has none;
    // every map carries the radius the bundle baked at.
    CHECK(fileOf("mesh").padding.radius == 0);
    // `const char*`, not `const std::string&`: binding a string reference to a
    // string-literal element constructs a temporary per iteration, which GCC
    // and the NDK's Clang reject under -Wrange-loop-construct.
    for (const char* kind : {"object-normal", "object-position", "bent-normal", "thickness"}) {
        CHECK(fileOf(kind).padding.radius == params.paddingRadius);
    }

    for (const bundle::BundleFile& file : result.files) {
        CHECK(fs::exists(file.path));
    }
}

TEST_CASE("a preset declaring an unknown up axis is reported, not guessed at") {
    const fs::path dir = testDir("unknown_up_axis");
    io::ExportPreset preset = smallPreset("odd-axis", io::GreenChannel::PlusY);
    preset.upAxis = "sideways";
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::ObjectNormal, io::ColorSpace::Linear, "object-normal"},
    };

    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.0f);
    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE(result.ok);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("sideways") != std::string::npos);
    CHECK(result.files.back().encoding.upAxis == cyber::bake::UpAxis::YUp);

    // A preset that asks for no object-space map has no axis to get wrong.
    const fs::path quiet = testDir("unknown_up_axis_quiet");
    io::ExportPreset plain = preset;
    plain.maps = {io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "normal"}};
    Mesh low2 = makeSurface(0.0f);
    CHECK(bundle::writeBundle(low2, high, paramsFor(plain, quiet)).warnings.empty());
}

TEST_CASE("a bundle writes the id maps with their table, and refuses to gamma them") {
    const fs::path dir = testDir("id_maps");
    io::ExportPreset preset = smallPreset("id-maps", io::GreenChannel::PlusY);
    // sRGB on an id map is the trap: linearToSrgb rewrites every byte, so the
    // written file would no longer match the table reported beside it.
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::MaterialId, io::ColorSpace::Srgb, "material-id"},
        io::PresetMapEntry{io::PresetMap::ObjectId, io::ColorSpace::Linear, "object-id"},
    };

    Mesh low = makeSurface(0.0f);
    Mesh high = makeSurface(0.0f);
    auto& materials = high.faceAttributes().create<std::int32_t>("material_id");
    materials[0] = 12;

    const bundle::BundleResult result = bundle::writeBundle(low, high, paramsFor(preset, dir));
    REQUIRE(result.ok);
    CHECK(kindsOf(result) == std::vector<std::string>{"mesh", "material-id", "object-id"});

    const bundle::BundleFile& material = result.files[1];
    CHECK(material.encoding.basis == cyber::bake::EncodingBasis::IdColor);
    CHECK(material.encoding.idSource == "material_id");
    REQUIRE(material.encoding.idColors.size() == 1);
    CHECK(material.encoding.idColors[0].id == 12);
    CHECK(material.encoding.idColors[0].color == cyber::bake::idColor(12));
    // Written linear despite the sRGB request, and the refusal is reported.
    CHECK(material.colorSpace == "linear");
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("material-id") != std::string::npos);

    // The bytes on disk are the table's bytes, at zero tolerance.
    const auto loaded = cyber::imageio::loadPng(material.path);
    REQUIRE(loaded.has_value());
    const std::array<std::uint8_t, 3> want = cyber::bake::idColor(12);
    const int channels = loaded->channels;
    bool sawColor = false;
    const std::size_t texels =
        static_cast<std::size_t>(loaded->width) * static_cast<std::size_t>(loaded->height);
    for (std::size_t i = 0; i < texels; ++i) {
        std::array<int, 3> got{};
        for (int c = 0; c < 3; ++c) {
            got[static_cast<std::size_t>(c)] = static_cast<int>(std::lround(
                loaded
                    ->pixels[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] *
                255.0f));
        }
        if (got == std::array<int, 3>{0, 0, 0}) {
            continue;  // the reserved "no id" padding
        }
        REQUIRE(got == std::array<int, 3>{want[0], want[1], want[2]});
        sawColor = true;
    }
    CHECK(sawColor);

    // The object map falls back to components: one surface, one id.
    const bundle::BundleFile& object = result.files[2];
    CHECK(object.encoding.idSource == "component");
    REQUIRE(object.encoding.idColors.size() == 1);
    CHECK(object.encoding.idColors[0].id == 0);
    fs::remove_all(dir);
}

TEST_CASE("a bundle pads each map by the rule its channel semantics ask for") {
    // The maps above are baked from an UNWRAPPED low-poly whose chart fills the
    // layout, so nothing is outside an island to fill. Here the low-poly brings
    // its own UVs, shrunk into a quarter of the layout, so the band is real and
    // the per-basis fill rule is observable.
    const fs::path dir = testDir("padrule");
    io::ExportPreset preset = smallPreset("padrule", io::GreenChannel::PlusY);
    preset.maps = {
        io::PresetMapEntry{io::PresetMap::Normal, io::ColorSpace::Linear, "normal"},
        io::PresetMapEntry{io::PresetMap::Curvature, io::ColorSpace::Linear, "curvature"},
        io::PresetMapEntry{io::PresetMap::MaterialId, io::ColorSpace::Linear, "material-id"},
    };

    Mesh low = makeSurface(0.0f);
    auto& uv = low.cornerAttributes().create<cyber::Vec2>("uv");
    for (Index fi = 0; fi < low.faceCapacity(); ++fi) {
        if (!low.isAlive(cyber::FaceId{fi})) {
            continue;
        }
        for (const cyber::LoopId l : low.faceLoops(cyber::FaceId{fi})) {
            const Vec3 pos = low.position(low.loopVertex(l));
            uv[l.value] = {pos.x * 0.5f, pos.y * 0.5f};
        }
    }
    const Mesh high = makeSurface(0.0f);
    const bundle::BundleParams params = paramsFor(preset, dir);
    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    REQUIRE(result.ok);

    const auto fileOf = [&](const std::string& kind) {
        for (const bundle::BundleFile& file : result.files) {
            if (file.kind == kind) {
                return file;
            }
        }
        FAIL("missing map " << kind);
        return bundle::BundleFile{};
    };
    CHECK(fileOf("normal").padding.mode == cyber::bake::PaddingMode::ExtrapolateUnit);
    CHECK(fileOf("curvature").padding.mode == cyber::bake::PaddingMode::Extrapolate);
    // The one rule that cannot be got wrong: an interpolated id colour resolves
    // to no id.
    CHECK(fileOf("material-id").padding.mode == cyber::bake::PaddingMode::Nearest);
    for (const char* kind : {"normal", "curvature", "material-id"}) {
        CHECK(fileOf(kind).padding.texelsFilled > 0);
    }

    // A zero radius turns the stage off for the whole bundle.
    bundle::BundleParams off = params;
    off.paddingRadius = 0;
    off.meshPath = dir / "off.obj";
    Mesh lowAgain = low;
    const bundle::BundleResult unpadded = bundle::writeBundle(lowAgain, high, off);
    REQUIRE(unpadded.ok);
    for (const bundle::BundleFile& file : unpadded.files) {
        CHECK(file.padding.radius == 0);
        CHECK(file.padding.texelsFilled == 0);
        CHECK(file.padding.mode == cyber::bake::PaddingMode::None);
    }
}

// ---- UDIM (surface-baking, "UDIM-aware baking"; mesh-io, "{udim}") --------

namespace {

// Two coplanar quads whose UVs sit in tiles 1001 and 1002. Explicit UVs, so the
// bundle takes the layout as authored instead of unwrapping it into one tile.
Mesh twoTileSurface(float z) {
    const std::vector<Vec3> p = {{0, 0, z}, {1, 0, z}, {1, 1, z}, {0, 1, z},
                                 {2, 0, z}, {3, 0, z}, {3, 1, z}, {2, 1, z}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    Mesh mesh = Mesh::fromIndexed(p, faces);
    auto& uv = mesh.cornerAttributes().create<cyber::Vec2>(io::kUvAttribute);
    const std::array<cyber::Vec2, 4> corners{cyber::Vec2{0.1f, 0.1f}, cyber::Vec2{0.9f, 0.1f},
                                             cyber::Vec2{0.9f, 0.9f}, cyber::Vec2{0.1f, 0.9f}};
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const cyber::FaceId face{fi};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const float shift = fi == 0 ? 0.0f : 1.0f;  // face 1 goes to tile 1002
        std::size_t corner = 0;
        for (const cyber::LoopId l : mesh.faceLoops(face)) {
            uv[l.value] = corners[corner] + cyber::Vec2{shift, 0.0f};
            ++corner;
        }
    }
    return mesh;
}

}  // namespace

TEST_CASE("a bundle reports the occupied tiles even when it is not UDIM-aware") {
    const fs::path dir = testDir("udim_report");
    Mesh low = twoTileSurface(0.0f);
    const Mesh high = twoTileSurface(0.02f);

    const bundle::BundleResult result =
        bundle::writeBundle(low, high, paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir));
    REQUIRE(result.ok);
    CHECK(result.udimTiles == std::vector<int>{1001, 1002});
    CHECK(result.udimUnaddressableFaces == 0);
    // Not UDIM-aware: still one file per map, and the row names tile 1001,
    // because the unit square IS that tile.
    CHECK(kindsOf(result) == std::vector<std::string>{"mesh", "normal", "curvature"});
    for (const bundle::BundleFile& file : result.files) {
        CHECK(file.udimTile == 1001);
    }
    fs::remove_all(dir);
}

TEST_CASE("a UDIM bundle writes one file per tile, named by the tile token") {
    const fs::path dir = testDir("udim_write");
    Mesh low = twoTileSurface(0.0f);
    const Mesh high = twoTileSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.namingPattern = "{basename}_{map}.{udim}.{ext}";
    bundle::BundleParams params = paramsFor(preset, dir);
    params.udim = true;

    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    REQUIRE(result.ok);
    CHECK(result.udimTiles == std::vector<int>{1001, 1002});
    REQUIRE(result.files.size() == 5);  // the mesh plus two maps in two tiles
    CHECK(fs::exists(dir / "hero_normal.1001.png"));
    CHECK(fs::exists(dir / "hero_normal.1002.png"));
    CHECK(fs::exists(dir / "hero_curvature.1001.png"));
    CHECK(fs::exists(dir / "hero_curvature.1002.png"));

    std::vector<int> tiles;
    for (const bundle::BundleFile& file : result.files) {
        if (file.kind == "normal") {
            tiles.push_back(file.udimTile);
        }
    }
    CHECK(tiles == std::vector<int>{1001, 1002});
    fs::remove_all(dir);
}

TEST_CASE("a multi-tile bundle through a pattern without the tile token is refused") {
    const fs::path dir = testDir("udim_no_token");
    Mesh low = twoTileSurface(0.0f);
    const Mesh high = twoTileSurface(0.02f);

    // The default pattern carries no {udim}: both tiles would land on one path.
    bundle::BundleParams params = paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir);
    params.udim = true;

    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("{udim}") != std::string::npos);
    CHECK(result.error.find(params.preset.namingPattern) != std::string::npos);
    // Refused BEFORE anything was written, so no half-set is left behind.
    CHECK_FALSE(fs::exists(dir / "hero.obj"));
    CHECK(result.files.empty());
    fs::remove_all(dir);
}

TEST_CASE("a single-tile UDIM bundle needs no tile token") {
    const fs::path dir = testDir("udim_single");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    bundle::BundleParams params = paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir);
    params.udim = true;

    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    REQUIRE(result.ok);
    CHECK(result.udimTiles == std::vector<int>{1001});
    CHECK(kindsOf(result) == std::vector<std::string>{"mesh", "normal", "curvature"});
    CHECK(fs::exists(dir / "hero_normal.png"));
    fs::remove_all(dir);
}

// ---- the texel ceiling on the bundle path --------------------------------
//
// The ceiling is the EMBEDDER's policy (the C ABI sets it from
// cyber_max_bake_pixels()), and a UDIM bundle multiplies the exposure by the
// occupied-tile count -- up to the whole addressable grid times resolution^2 --
// so it has to bind here and not only in bake::bakeUdim().

TEST_CASE("a bundle whose preset is over the per-tile ceiling is refused, writing nothing") {
    const fs::path dir = testDir("udim_per_tile_ceiling");
    Mesh low = makeSurface(0.0f);
    const Mesh high = makeSurface(0.02f);

    bundle::BundleParams params = paramsFor(smallPreset("t", io::GreenChannel::PlusY), dir);
    params.maxPixels = 32 * 32 - 1;  // below the preset's own 32x32

    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("PER-TILE") != std::string::npos);
    CHECK(result.error.find(params.preset.name) != std::string::npos);
    // Refused before anything was written: not even the mesh.
    CHECK(result.files.empty());
    CHECK_FALSE(fs::exists(dir / "hero.obj"));
    fs::remove_all(dir);
}

TEST_CASE("a two-tile UDIM bundle is refused by the aggregate ceiling one map would pass") {
    const fs::path dir = testDir("udim_aggregate_ceiling");
    Mesh low = twoTileSurface(0.0f);
    const Mesh high = twoTileSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.namingPattern = "{basename}_{map}.{udim}.{ext}";
    bundle::BundleParams params = paramsFor(preset, dir);
    params.udim = true;
    params.maxPixels = 32 * 32 + 1;  // one tile fits, two do not

    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("AGGREGATE") != std::string::npos);
    CHECK(result.error.find('2') != std::string::npos);  // the tile count it was asked for
    CHECK(result.files.empty());
    CHECK_FALSE(fs::exists(dir / "hero.obj"));

    // The same layout and the same ceiling WITHOUT --udim is one image, and one
    // image fits: the aggregate ceiling counts the tiles actually baked.
    const fs::path plain = testDir("udim_aggregate_ceiling_plain");
    bundle::BundleParams single = paramsFor(preset, plain);
    single.maxPixels = params.maxPixels;
    Mesh lowAgain = twoTileSurface(0.0f);
    const bundle::BundleResult ok = bundle::writeBundle(lowAgain, high, single);
    CHECK(ok.ok);
    fs::remove_all(plain);
    fs::remove_all(dir);
}

TEST_CASE("a bundle with no ceiling bakes whatever the preset asks for") {
    const fs::path dir = testDir("udim_no_ceiling");
    Mesh low = twoTileSurface(0.0f);
    const Mesh high = twoTileSurface(0.02f);

    io::ExportPreset preset = smallPreset("t", io::GreenChannel::PlusY);
    preset.namingPattern = "{basename}_{map}.{udim}.{ext}";
    bundle::BundleParams params = paramsFor(preset, dir);
    params.udim = true;  // maxPixels stays 0: no ceiling, the CLI's case

    const bundle::BundleResult result = bundle::writeBundle(low, high, params);
    CHECK(result.ok);
    CHECK(result.files.size() == 5);
    fs::remove_all(dir);
}
