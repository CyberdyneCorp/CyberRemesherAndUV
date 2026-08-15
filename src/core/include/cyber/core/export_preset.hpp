#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cyber/core/io.hpp"

// Named per-DCC export presets (mesh-io spec, "Named export presets"). A preset
// is pure DATA describing an export bundle: which maps to emit, how to name
// them, what color space and normal convention the target app expects, and
// which mesh format to write. It deliberately does NOT reference the bake
// module -- core knows nothing about baking, and the consumer (the CLI) maps
// PresetMap onto its own bake calls.
namespace cyber::io {

// Bumped whenever a schema change would alter the CONTENT of an export. A
// preset file declaring anything else is rejected with a typed error rather
// than partially honored (mesh-io spec, "Preset schema is versioned and fails
// loudly" -- a competitor's engine rewrite silently destroyed user preset
// libraries, so silence is the one outcome ruled out).
inline constexpr int kPresetSchemaVersion = 1;

// The map kinds a preset can request. A superset of what any single built-in
// uses, so user presets can name anything the engine can bake.
enum class PresetMap {
    Normal,
    AmbientOcclusion,
    Curvature,
    Cavity,
    Displacement,
    Color,
    Position,
};

enum class ColorSpace {
    Linear,
    Srgb,
};

// Normal-map green-channel convention. OpenGL-style (+Y, green points up) is
// what Blender, Unity and glTF expect; DirectX-style (-Y) is what Unreal
// expects. The difference is a per-texel green flip at write time.
enum class GreenChannel {
    PlusY,
    MinusY,
};

struct PresetMapEntry {
    PresetMap map = PresetMap::Normal;
    ColorSpace colorSpace = ColorSpace::Linear;
    // Token substituted for {map} in the naming pattern. Defaults to the map's
    // canonical name; a preset may override it to match a target app's
    // suffix conventions (Unreal's "_N", say).
    std::string suffix;
};

struct ExportPreset {
    int schemaVersion = kPresetSchemaVersion;
    std::string name;
    // Mesh container extension WITHOUT the dot: obj | ply | stl | gltf | glb.
    std::string meshFormat = "obj";
    // Texture container extension without the dot: png | exr.
    std::string textureFormat = "png";
    // Tokens: {basename}, {map}, {preset}, {ext}.
    std::string namingPattern = "{basename}_{map}.{ext}";
    GreenChannel normalGreen = GreenChannel::PlusY;
    int resolution = 2048;
    // Metadata recorded in the report and applied only where the chosen mesh
    // format actually carries it (glTF fixes both; OBJ/PLY/STL carry neither).
    std::string units = "meters";
    std::string upAxis = "y-up";
    std::vector<PresetMapEntry> maps;
};

// Names of the shipped built-ins, in a stable order for help text.
[[nodiscard]] std::vector<std::string> builtinPresetNames();

// A built-in by name, or nullopt when the name is not built in.
[[nodiscard]] std::optional<ExportPreset> builtinPreset(std::string_view name);

// Parses a preset from JSON text. Fails with ErrorCode::IncompatibleVersion on
// a schema version this engine does not support (naming both versions), and
// with ErrorCode::ParseError on malformed or contradictory content. Unknown
// fields are rejected rather than ignored: silently dropping a field that was
// meant to change the output is exactly the failure mode this guards.
[[nodiscard]] Result<ExportPreset> parsePreset(std::string_view json,
                                               std::string_view originLabel = "<memory>");

// Loads a preset file from disk.
[[nodiscard]] Result<ExportPreset> loadPreset(const std::filesystem::path& path);

// Resolves a `--preset` argument: a built-in name if one matches, otherwise a
// path to a user preset file. A name that is neither is an UnsupportedFormat
// error listing the built-ins.
[[nodiscard]] Result<ExportPreset> resolvePreset(const std::string& nameOrPath);

// Expands `preset.namingPattern` for one map entry. The result is always a
// relative name inside the caller's output directory: an expansion that would
// escape it -- an absolute or ".."-climbing value arriving through ANY token,
// including a caller-supplied `basename` -- yields an empty string, which
// callers must treat as a refusal rather than as a file name.
[[nodiscard]] std::string presetMapFileName(const ExportPreset& preset, const PresetMapEntry& entry,
                                            std::string_view basename);

// Canonical spelling of a map kind, as it appears in preset JSON and reports.
[[nodiscard]] const char* presetMapName(PresetMap map);
[[nodiscard]] std::optional<PresetMap> presetMapFromName(std::string_view name);
[[nodiscard]] const char* colorSpaceName(ColorSpace space);

// Encodes a linear value to sRGB (IEC 61966-2-1). Applied to maps a preset
// declares sRGB before they are written to an 8-bit container, which stores
// values verbatim.
[[nodiscard]] float linearToSrgb(float linear);

}  // namespace cyber::io
