#include <doctest.h>

#include <algorithm>
#include <string>

#include "cyber/core/export_preset.hpp"

namespace io = cyber::io;

namespace {

// A minimal valid preset; individual cases override one field at a time so a
// failure names the field that broke rather than "the JSON".
std::string presetJson(const std::string& body) {
    return "{\"schemaVersion\":1,\"name\":\"t\"," + body + "}";
}

bool hasBuiltin(const std::string& name) {
    const std::vector<std::string> names = io::builtinPresetNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

// ---- built-ins ----------------------------------------------------------

TEST_CASE("the four documented presets ship built in") {
    REQUIRE(io::builtinPresetNames().size() == 4);
    REQUIRE(hasBuiltin("blender"));
    REQUIRE(hasBuiltin("unity"));
    REQUIRE(hasBuiltin("unreal"));
    REQUIRE(hasBuiltin("gltf-generic"));
    REQUIRE_FALSE(io::builtinPreset("nosuch").has_value());
}

TEST_CASE("only the unreal preset uses the DirectX green channel") {
    // Getting this backwards inverts every dent on the surface, so it is worth
    // pinning per preset rather than trusting the table.
    REQUIRE(io::builtinPreset("unreal")->normalGreen == io::GreenChannel::MinusY);
    REQUIRE(io::builtinPreset("blender")->normalGreen == io::GreenChannel::PlusY);
    REQUIRE(io::builtinPreset("unity")->normalGreen == io::GreenChannel::PlusY);
    REQUIRE(io::builtinPreset("gltf-generic")->normalGreen == io::GreenChannel::PlusY);
}

TEST_CASE("built-in map sets carry color as the only sRGB map") {
    for (const std::string& name : io::builtinPresetNames()) {
        const io::ExportPreset preset = *io::builtinPreset(name);
        REQUIRE_FALSE(preset.maps.empty());
        for (const io::PresetMapEntry& entry : preset.maps) {
            const bool isColor = entry.map == io::PresetMap::Color;
            // Data maps must stay linear: a gamma curve on a normal or AO map
            // is a bug in every target app.
            REQUIRE(((entry.colorSpace == io::ColorSpace::Srgb) == isColor));
        }
    }
}

TEST_CASE("gltf-generic sticks to slots glTF 2.0 actually has") {
    const io::ExportPreset preset = *io::builtinPreset("gltf-generic");
    for (const io::PresetMapEntry& entry : preset.maps) {
        REQUIRE(entry.map != io::PresetMap::Curvature);
        REQUIRE(entry.map != io::PresetMap::Cavity);
    }
}

// ---- schema versioning --------------------------------------------------

TEST_CASE("an incompatible schema version is rejected loudly, naming both") {
    const auto result = io::parsePreset(R"({"schemaVersion":99,"name":"t","maps":["normal"]})");
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().code == io::ErrorCode::IncompatibleVersion);
    // The message must name the found AND the supported version — a bare
    // "unsupported preset" leaves the author with nothing to act on.
    REQUIRE(result.error().message.find("99") != std::string::npos);
    REQUIRE(result.error().message.find('1') != std::string::npos);
}

TEST_CASE("a missing schema version is an error, not a default") {
    const auto result = io::parsePreset(R"({"name":"t","maps":["normal"]})");
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("version is checked before any other field") {
    // Malformed everywhere AND from the future: the version must win, so the
    // author is told the real problem rather than a downstream symptom.
    const auto result = io::parsePreset(R"({"schemaVersion":7,"maps":42})");
    REQUIRE(result.error().code == io::ErrorCode::IncompatibleVersion);
}

TEST_CASE("an unknown field is rejected rather than ignored") {
    const auto result = io::parsePreset(presetJson(R"("maps":["normal"],"glossiness":true)"));
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().message.find("glossiness") != std::string::npos);
}

// ---- content ------------------------------------------------------------

TEST_CASE("a valid preset round-trips its fields") {
    const auto result = io::parsePreset(presetJson(
        R"("meshFormat":"glb","textureFormat":"exr","normalGreen":"-Y","resolution":512,)"
        R"("namingPattern":"{preset}-{map}.{ext}","units":"centimeters","upAxis":"z-up",)"
        R"("maps":["normal",{"map":"color","colorSpace":"srgb","suffix":"basecolor"}])"));
    REQUIRE(result.ok());
    const io::ExportPreset& preset = result.value();
    REQUIRE(preset.meshFormat == "glb");
    REQUIRE(preset.textureFormat == "exr");
    REQUIRE(preset.normalGreen == io::GreenChannel::MinusY);
    REQUIRE(preset.resolution == 512);
    REQUIRE(preset.units == "centimeters");
    REQUIRE(preset.maps.size() == 2);
    REQUIRE(preset.maps[0].map == io::PresetMap::Normal);
    REQUIRE(preset.maps[0].colorSpace == io::ColorSpace::Linear);
    REQUIRE(preset.maps[1].suffix == "basecolor");
}

TEST_CASE("a bare map name defaults color to sRGB and everything else to linear") {
    const auto result = io::parsePreset(presetJson(R"("maps":["color","ao","curvature"])"));
    REQUIRE(result.ok());
    REQUIRE(result.value().maps[0].colorSpace == io::ColorSpace::Srgb);
    REQUIRE(result.value().maps[1].colorSpace == io::ColorSpace::Linear);
    REQUIRE(result.value().maps[2].colorSpace == io::ColorSpace::Linear);
}

TEST_CASE("malformed content is rejected") {
    REQUIRE_FALSE(io::parsePreset("not json").ok());
    REQUIRE_FALSE(io::parsePreset(presetJson(R"("maps":[])")).ok());
    REQUIRE_FALSE(io::parsePreset(presetJson(R"("maps":["nosuchmap"])")).ok());
    REQUIRE_FALSE(io::parsePreset(presetJson(R"("maps":["normal"],"resolution":0)")).ok());
    // A pattern without {map} would have every map overwrite the previous one.
    REQUIRE_FALSE(
        io::parsePreset(presetJson(R"("maps":["normal","ao"],"namingPattern":"{basename}.{ext}")"))
            .ok());
    REQUIRE(io::parsePreset(presetJson(R"("maps":["normal"],"meshFormat":"fbx")")).error().code ==
            io::ErrorCode::UnsupportedFormat);
    REQUIRE(
        io::parsePreset(presetJson(R"("maps":["normal"],"textureFormat":"tga")")).error().code ==
        io::ErrorCode::UnsupportedFormat);
}

TEST_CASE("a wrong-typed field is a typed error, not a thrown json exception") {
    // Regression: the optional fields were read with get<T>() straight after a
    // contains() check, so a wrong type escaped as nlohmann::json::type_error
    // out of a function documented to report malformed content through Result.
    // A consumer following that contract has no catch handler and terminates.
    struct Case {
        const char* body;
        const char* field;
    };
    const std::vector<Case> cases = {
        {R"("maps":["normal"],"resolution":"2048")", "resolution"},
        {R"("maps":["normal"],"resolution":512.5)", "resolution"},
        {R"("maps":["normal"],"meshFormat":7)", "meshFormat"},
        {R"("maps":["normal"],"textureFormat":1)", "textureFormat"},
        {R"("maps":["normal"],"namingPattern":5)", "namingPattern"},
        {R"("maps":["normal"],"normalGreen":[1])", "normalGreen"},
        {R"("maps":["normal"],"units":{})", "units"},
        {R"("maps":["normal"],"upAxis":2)", "upAxis"},
        {R"("maps":[{"map":"normal","colorSpace":3}])", "colorSpace"},
        {R"("maps":[{"map":"normal","suffix":3}])", "suffix"},
    };
    for (const Case& c : cases) {
        INFO("field: " << c.field);
        const auto result = io::parsePreset(presetJson(c.body));
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.error().code == io::ErrorCode::ParseError);
        // The message has to name the offending field, or the author is left
        // guessing which line of their preset is wrong.
        REQUIRE(result.error().message.find(c.field) != std::string::npos);
    }
}

TEST_CASE("a naming pattern cannot escape the output directory") {
    // Regression: the pattern was validated only for {map}, so a preset FILE
    // chose where the engine wrote — an absolute pattern redirected every map
    // to a path of the preset author's choosing, and ".." climbed out of the
    // caller's --output directory. A pattern is a name, never a destination.
    const auto absolute = io::parsePreset(
        presetJson(R"("maps":["normal"],"namingPattern":"/tmp/pwned_{map}.{ext}")"));
    REQUIRE_FALSE(absolute.ok());
    REQUIRE(absolute.error().code == io::ErrorCode::ParseError);
    REQUIRE(absolute.error().message.find("namingPattern") != std::string::npos);

    const auto climbing =
        io::parsePreset(presetJson(R"("maps":["normal"],"namingPattern":"../../{map}.{ext}")"));
    REQUIRE_FALSE(climbing.ok());
    REQUIRE(climbing.error().code == io::ErrorCode::ParseError);

    // A map suffix is substituted into the same file name, so it escapes the
    // same way if left unchecked.
    const auto suffix =
        io::parsePreset(presetJson(R"("maps":[{"map":"normal","suffix":"../../evil"}])"));
    REQUIRE_FALSE(suffix.ok());
    REQUIRE(suffix.error().code == io::ErrorCode::ParseError);

    // A relative subdirectory is still a legal name inside the output.
    REQUIRE(io::parsePreset(
                presetJson(R"("maps":["normal"],"namingPattern":"maps/{basename}_{map}.{ext}")"))
                .ok());
}

TEST_CASE("a preset name cannot escape the output directory through {preset}") {
    // Regression: the traversal gate ran on the RAW namingPattern, so a preset
    // whose pattern was a legal relative name smuggled the escape in through
    // the "name" field that {preset} expands — the parse succeeded and the
    // public file-name accessor handed a climbing path to the host.
    const auto climbing = io::parsePreset(
        R"({"schemaVersion":1,"name":"../../../tmp/pwned","maps":["normal"],)"
        R"("namingPattern":"{preset}_{map}.{ext}"})");
    REQUIRE_FALSE(climbing.ok());
    REQUIRE(climbing.error().code == io::ErrorCode::ParseError);
    REQUIRE(climbing.error().message.find("name") != std::string::npos);

    const auto absolute =
        io::parsePreset(R"({"schemaVersion":1,"name":"/tmp/pwned","maps":["normal"]})");
    REQUIRE_FALSE(absolute.ok());
    REQUIRE(absolute.error().code == io::ErrorCode::ParseError);

    // Names that merely contain dots or dashes are ordinary names.
    REQUIRE(io::parsePreset(R"({"schemaVersion":1,"name":"studio-v1.2","maps":["normal"]})").ok());
}

TEST_CASE("an expanded map file name that escapes the output directory is refused") {
    // The containment check lives on the expansion, not on one token, so a
    // value arriving through ANY token — including a caller-supplied basename
    // that never passes through the parser — is refused with an empty name.
    io::ExportPreset preset = *io::builtinPreset("blender");
    REQUIRE(io::presetMapFileName(preset, preset.maps[0], "hero") == "hero_normal.png");
    REQUIRE(io::presetMapFileName(preset, preset.maps[0], "../../hero").empty());
    REQUIRE(io::presetMapFileName(preset, preset.maps[0], "/tmp/hero").empty());

    // Presets built in C++ never went through parsePreset, so the same escape
    // is possible through the name and the suffix.
    preset.namingPattern = "{preset}_{map}.{ext}";
    preset.name = "../../../tmp/pwned";
    REQUIRE(io::presetMapFileName(preset, preset.maps[0], "hero").empty());

    preset.name = "blender";
    preset.namingPattern = "{map}.{ext}";
    io::PresetMapEntry entry = preset.maps[0];
    entry.suffix = "../../evil";
    REQUIRE(io::presetMapFileName(preset, entry, "hero").empty());

    // A subdirectory inside the output directory is still a legal expansion.
    preset.namingPattern = "{preset}/{basename}_{map}.{ext}";
    REQUIRE(io::presetMapFileName(preset, preset.maps[0], "hero") == "blender/hero_normal.png");
}

// ---- resolution and naming ---------------------------------------------

TEST_CASE("resolvePreset prefers a built-in name and lists them when unknown") {
    REQUIRE(io::resolvePreset("unreal").ok());
    const auto missing = io::resolvePreset("nosuch");
    REQUIRE_FALSE(missing.ok());
    REQUIRE(missing.error().code == io::ErrorCode::UnsupportedFormat);
    // The error has to be actionable: it names what IS available.
    REQUIRE(missing.error().message.find("blender") != std::string::npos);
    REQUIRE(missing.error().message.find("gltf-generic") != std::string::npos);
}

TEST_CASE("the naming pattern expands every token") {
    io::ExportPreset preset = *io::builtinPreset("blender");
    preset.namingPattern = "{preset}/{basename}_{map}.{ext}";
    REQUIRE(io::presetMapFileName(preset, preset.maps[0], "hero") == "blender/hero_normal.png");
}

TEST_CASE("a map suffix overrides the canonical name in the pattern") {
    io::ExportPreset preset = *io::builtinPreset("blender");
    io::PresetMapEntry entry = preset.maps[0];
    entry.suffix = "N";
    REQUIRE(io::presetMapFileName(preset, entry, "hero") == "hero_N.png");
}

TEST_CASE("every built-in names its maps distinctly") {
    for (const std::string& name : io::builtinPresetNames()) {
        const io::ExportPreset preset = *io::builtinPreset(name);
        std::vector<std::string> paths;
        for (const io::PresetMapEntry& entry : preset.maps) {
            paths.push_back(io::presetMapFileName(preset, entry, "m"));
        }
        std::sort(paths.begin(), paths.end());
        REQUIRE(std::unique(paths.begin(), paths.end()) == paths.end());
    }
}

// ---- color space --------------------------------------------------------

TEST_CASE("linearToSrgb matches the IEC 61966-2-1 curve at its anchors") {
    REQUIRE(io::linearToSrgb(0.0f) == doctest::Approx(0.0f));
    REQUIRE(io::linearToSrgb(1.0f) == doctest::Approx(1.0f));
    // Mid-gray: linear 0.2140 is sRGB 0.5, the standard's reference point.
    REQUIRE(io::linearToSrgb(0.2140f) == doctest::Approx(0.5f).epsilon(0.01));
    // The linear segment below the 0.0031308 knee.
    REQUIRE(io::linearToSrgb(0.001f) == doctest::Approx(0.01292f).epsilon(0.001));
    // Monotonic and clamped.
    REQUIRE(io::linearToSrgb(-1.0f) == doctest::Approx(0.0f));
    REQUIRE(io::linearToSrgb(5.0f) == doctest::Approx(1.0f));
}
