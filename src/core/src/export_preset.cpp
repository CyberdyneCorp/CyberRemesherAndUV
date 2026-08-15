#include "cyber/core/export_preset.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <json.hpp>
#include <sstream>
#include <unordered_set>

namespace cyber::io {

namespace {

using nlohmann::json;

Error parseError(std::string message) { return Error{ErrorCode::ParseError, std::move(message)}; }

struct MapNaming {
    PresetMap map;
    const char* name;
};

constexpr std::array<MapNaming, 7> kMapNames{{
    {PresetMap::Normal, "normal"},
    {PresetMap::AmbientOcclusion, "ao"},
    {PresetMap::Curvature, "curvature"},
    {PresetMap::Cavity, "cavity"},
    {PresetMap::Displacement, "displacement"},
    {PresetMap::Color, "color"},
    {PresetMap::Position, "position"},
}};

PresetMapEntry entryOf(PresetMap map, ColorSpace space) {
    PresetMapEntry entry;
    entry.map = map;
    entry.colorSpace = space;
    entry.suffix = presetMapName(map);
    return entry;
}

// The standard pre-texture set: normal + occlusion + curvature, plus base
// color. Only color is sRGB -- every other map carries data, not appearance,
// and a gamma curve on it is a bug in every target app.
std::vector<PresetMapEntry> standardMaps() {
    return {
        entryOf(PresetMap::Normal, ColorSpace::Linear),
        entryOf(PresetMap::AmbientOcclusion, ColorSpace::Linear),
        entryOf(PresetMap::Curvature, ColorSpace::Linear),
        entryOf(PresetMap::Color, ColorSpace::Srgb),
    };
}

ExportPreset makeBlender() {
    ExportPreset p;
    p.name = "blender";
    p.meshFormat = "obj";
    p.normalGreen = GreenChannel::PlusY;  // Blender reads OpenGL-style normals
    p.upAxis = "y-up";                    // what an OBJ written by this engine is
    p.maps = standardMaps();
    return p;
}

ExportPreset makeUnity() {
    ExportPreset p;
    p.name = "unity";
    p.meshFormat = "glb";
    p.normalGreen = GreenChannel::PlusY;  // Unity expects OpenGL-style normals
    p.maps = standardMaps();
    return p;
}

ExportPreset makeUnreal() {
    ExportPreset p;
    p.name = "unreal";
    p.meshFormat = "glb";
    // Unreal expects DirectX-style normals: green points down. This is the one
    // built-in that flips, and getting it wrong inverts every surface dent.
    p.normalGreen = GreenChannel::MinusY;
    p.units = "centimeters";  // Unreal's world unit
    p.maps = standardMaps();
    return p;
}

ExportPreset makeGltfGeneric() {
    ExportPreset p;
    p.name = "gltf-generic";
    p.meshFormat = "glb";
    p.normalGreen = GreenChannel::PlusY;  // fixed by the glTF 2.0 spec
    // glTF 2.0 core has no curvature slot, so the generic bundle stays to what
    // the format can actually describe: normal, occlusion and base color.
    p.maps = {
        entryOf(PresetMap::Normal, ColorSpace::Linear),
        entryOf(PresetMap::AmbientOcclusion, ColorSpace::Linear),
        entryOf(PresetMap::Color, ColorSpace::Srgb),
    };
    return p;
}

const std::vector<ExportPreset>& builtins() {
    static const std::vector<ExportPreset> presets{makeBlender(), makeUnity(), makeUnreal(),
                                                   makeGltfGeneric()};
    return presets;
}

std::string joinBuiltinNames() {
    std::string out;
    for (const std::string& name : builtinPresetNames()) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

// Every key the schema defines. Anything else is an error: a field the author
// expected to change the output must never be silently dropped.
const std::unordered_set<std::string>& knownKeys() {
    static const std::unordered_set<std::string> keys{
        "schemaVersion", "name", "meshFormat", "textureFormat", "namingPattern",
        "normalGreen",   "maps", "resolution", "units",         "upAxis",
    };
    return keys;
}

bool isSupportedMeshFormat(const std::string& ext) {
    return ext == "obj" || ext == "ply" || ext == "stl" || ext == "gltf" || ext == "glb";
}

Result<GreenChannel> parseGreen(const std::string& text, std::string_view origin) {
    if (text == "+Y" || text == "opengl") {
        return GreenChannel::PlusY;
    }
    if (text == "-Y" || text == "directx") {
        return GreenChannel::MinusY;
    }
    return parseError(std::string(origin) + ": normalGreen must be \"+Y\"/\"opengl\" or " +
                      "\"-Y\"/\"directx\", found \"" + text + "\"");
}

Result<ColorSpace> parseColorSpace(const std::string& text, std::string_view origin) {
    if (text == "linear") {
        return ColorSpace::Linear;
    }
    if (text == "srgb") {
        return ColorSpace::Srgb;
    }
    return parseError(std::string(origin) +
                      ": colorSpace must be \"linear\" or \"srgb\", found \"" + text + "\"");
}

// A value substituted into the file NAME has to stay a name: a separator or a
// ".." would move the map out of the caller's output directory exactly like an
// escaping namingPattern does. Applies to every preset field the pattern can
// expand -- a map suffix and the preset's own name.
bool isFileNameToken(const std::string& text) {
    return text != ".." && text.find('/') == std::string::npos &&
           text.find('\\') == std::string::npos;
}

// Reads the "maps" array. Each entry is either a bare map name or an object
// with `map` plus optional `colorSpace` / `suffix`.
Result<std::vector<PresetMapEntry>> parseMaps(const json& node, std::string_view origin) {
    if (!node.is_array() || node.empty()) {
        return parseError(std::string(origin) + ": \"maps\" must be a non-empty array");
    }
    std::vector<PresetMapEntry> maps;
    for (const json& item : node) {
        std::string mapName;
        std::optional<std::string> colorSpaceText;
        std::optional<std::string> suffix;
        if (item.is_string()) {
            mapName = item.get<std::string>();
        } else if (item.is_object()) {
            if (!item.contains("map") || !item["map"].is_string()) {
                return parseError(std::string(origin) +
                                  ": a \"maps\" entry needs a string \"map\"");
            }
            mapName = item["map"].get<std::string>();
            if (item.contains("colorSpace")) {
                if (!item["colorSpace"].is_string()) {
                    return parseError(std::string(origin) +
                                      ": a \"maps\" entry's \"colorSpace\" must be a string");
                }
                colorSpaceText = item["colorSpace"].get<std::string>();
            }
            if (item.contains("suffix")) {
                if (!item["suffix"].is_string()) {
                    return parseError(std::string(origin) +
                                      ": a \"maps\" entry's \"suffix\" must be a string");
                }
                suffix = item["suffix"].get<std::string>();
            }
        } else {
            return parseError(std::string(origin) +
                              ": a \"maps\" entry must be a string or an object");
        }

        const std::optional<PresetMap> map = presetMapFromName(mapName);
        if (!map.has_value()) {
            return parseError(std::string(origin) + ": unknown map \"" + mapName + "\"");
        }
        // Color is the only map whose natural container is sRGB; default the
        // rest to linear so an under-specified preset cannot gamma-encode data.
        PresetMapEntry entry =
            entryOf(*map, *map == PresetMap::Color ? ColorSpace::Srgb : ColorSpace::Linear);
        if (colorSpaceText.has_value()) {
            const Result<ColorSpace> space = parseColorSpace(*colorSpaceText, origin);
            if (!space.ok()) {
                return space.error();
            }
            entry.colorSpace = space.value();
        }
        if (suffix.has_value()) {
            if (!isFileNameToken(*suffix)) {
                return parseError(std::string(origin) + ": map suffix \"" + *suffix +
                                  "\" must be a plain file-name fragment");
            }
            entry.suffix = *suffix;
        }
        maps.push_back(std::move(entry));
    }
    return maps;
}

// Optional string field. Absent leaves `out` at its default; present but
// wrong-typed is a ParseError naming the field, because parsePreset's contract
// is to REPORT malformed content, not to throw a json exception through a
// caller that only checks Result::ok().
std::optional<Error> readString(const json& root, const char* key, std::string_view origin,
                                std::string& out) {
    if (!root.contains(key)) {
        return std::nullopt;
    }
    if (!root[key].is_string()) {
        return parseError(std::string(origin) + ": \"" + key + "\" must be a string");
    }
    out = root[key].get<std::string>();
    return std::nullopt;
}

std::optional<Error> readInt(const json& root, const char* key, std::string_view origin, int& out) {
    if (!root.contains(key)) {
        return std::nullopt;
    }
    if (!root[key].is_number_integer()) {
        return parseError(std::string(origin) + ": \"" + key + "\" must be an integer");
    }
    out = root[key].get<int>();
    return std::nullopt;
}

// A map file name lives INSIDE the caller's output directory. An absolute name
// or one with a `..` component makes a preset FILE decide where the engine
// writes, so a downloaded preset could overwrite anything the process can
// reach; a preset names files, never destinations. Checked both on the raw
// pattern (so a bad preset fails at parse, where the error is actionable) and
// on the fully expanded name (so no token can smuggle a separator past it).
bool escapesOutputDirectory(const std::string& name) {
    const std::filesystem::path path(name);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return true;
    }
    for (const std::filesystem::path& part : path) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

void replaceAll(std::string& text, std::string_view token, std::string_view value) {
    for (std::size_t at = text.find(token); at != std::string::npos;
         at = text.find(token, at + value.size())) {
        text.replace(at, token.size(), value);
    }
}

}  // namespace

const char* presetMapName(PresetMap map) {
    for (const MapNaming& entry : kMapNames) {
        if (entry.map == map) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<PresetMap> presetMapFromName(std::string_view name) {
    for (const MapNaming& entry : kMapNames) {
        if (name == entry.name) {
            return entry.map;
        }
    }
    return std::nullopt;
}

const char* colorSpaceName(ColorSpace space) {
    return space == ColorSpace::Srgb ? "srgb" : "linear";
}

float linearToSrgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    if (c <= 0.0031308f) {
        return 12.92f * c;
    }
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

std::vector<std::string> builtinPresetNames() {
    std::vector<std::string> names;
    names.reserve(builtins().size());
    for (const ExportPreset& preset : builtins()) {
        names.push_back(preset.name);
    }
    return names;
}

std::optional<ExportPreset> builtinPreset(std::string_view name) {
    for (const ExportPreset& preset : builtins()) {
        if (preset.name == name) {
            return preset;
        }
    }
    return std::nullopt;
}

Result<ExportPreset> parsePreset(std::string_view json_text, std::string_view origin) {
    json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        return parseError(std::string(origin) + ": not a JSON object");
    }

    // Version first: an incompatible file must be rejected on its contract, not
    // on whichever field happens to be malformed downstream.
    if (!root.contains("schemaVersion") || !root["schemaVersion"].is_number_integer()) {
        return parseError(std::string(origin) + ": missing integer \"schemaVersion\"");
    }
    const int version = root["schemaVersion"].get<int>();
    if (version != kPresetSchemaVersion) {
        return Error{ErrorCode::IncompatibleVersion,
                     std::string(origin) + ": preset schema version " + std::to_string(version) +
                         " is not supported (this engine supports version " +
                         std::to_string(kPresetSchemaVersion) + ")"};
    }

    for (const auto& item : root.items()) {
        if (knownKeys().count(item.key()) == 0) {
            return parseError(std::string(origin) + ": unknown field \"" + item.key() +
                              "\" (refusing to ignore a field that may change the output)");
        }
    }

    ExportPreset preset;
    preset.schemaVersion = version;
    if (!root.contains("name") || !root["name"].is_string()) {
        return parseError(std::string(origin) + ": missing string \"name\"");
    }
    preset.name = root["name"].get<std::string>();
    // The name is substituted for {preset} in the naming pattern, so it is a
    // file-name fragment on the same terms as a map suffix.
    if (!isFileNameToken(preset.name)) {
        return parseError(std::string(origin) + ": \"name\" \"" + preset.name +
                          "\" must be a plain file-name fragment (it is substituted for "
                          "{preset} in the naming pattern)");
    }

    if (const std::optional<Error> bad =
            readString(root, "meshFormat", origin, preset.meshFormat)) {
        return *bad;
    }
    if (!isSupportedMeshFormat(preset.meshFormat)) {
        return Error{ErrorCode::UnsupportedFormat,
                     std::string(origin) + ": unsupported meshFormat \"" + preset.meshFormat +
                         "\" (obj, ply, stl, gltf, glb)"};
    }
    if (const std::optional<Error> bad =
            readString(root, "textureFormat", origin, preset.textureFormat)) {
        return *bad;
    }
    if (preset.textureFormat != "png" && preset.textureFormat != "exr") {
        return Error{ErrorCode::UnsupportedFormat, std::string(origin) +
                                                       ": unsupported textureFormat \"" +
                                                       preset.textureFormat + "\" (png, exr)"};
    }
    if (const std::optional<Error> bad =
            readString(root, "namingPattern", origin, preset.namingPattern)) {
        return *bad;
    }
    if (preset.namingPattern.find("{map}") == std::string::npos) {
        return parseError(std::string(origin) +
                          ": namingPattern must contain {map}, or every map would "
                          "overwrite the previous one");
    }
    if (escapesOutputDirectory(preset.namingPattern)) {
        return parseError(std::string(origin) + ": namingPattern \"" + preset.namingPattern +
                          "\" must be a relative name inside the output directory (no leading "
                          "\"/\" and no \"..\")");
    }
    if (root.contains("normalGreen")) {
        std::string greenText;
        if (const std::optional<Error> bad = readString(root, "normalGreen", origin, greenText)) {
            return *bad;
        }
        const Result<GreenChannel> green = parseGreen(greenText, origin);
        if (!green.ok()) {
            return green.error();
        }
        preset.normalGreen = green.value();
    }
    if (const std::optional<Error> bad = readInt(root, "resolution", origin, preset.resolution)) {
        return *bad;
    }
    if (preset.resolution <= 0) {
        return parseError(std::string(origin) + ": resolution must be positive");
    }
    if (const std::optional<Error> bad = readString(root, "units", origin, preset.units)) {
        return *bad;
    }
    if (const std::optional<Error> bad = readString(root, "upAxis", origin, preset.upAxis)) {
        return *bad;
    }
    if (!root.contains("maps")) {
        return parseError(std::string(origin) + ": missing \"maps\"");
    }
    Result<std::vector<PresetMapEntry>> maps = parseMaps(root["maps"], origin);
    if (!maps.ok()) {
        return maps.error();
    }
    preset.maps = std::move(maps.value());
    return preset;
}

Result<ExportPreset> loadPreset(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return Error{ErrorCode::FileNotFound, "cannot open preset '" + path.string() + "'"};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parsePreset(buffer.str(), path.string());
}

Result<ExportPreset> resolvePreset(const std::string& nameOrPath) {
    if (const std::optional<ExportPreset> builtin = builtinPreset(nameOrPath)) {
        return *builtin;
    }
    const std::filesystem::path path(nameOrPath);
    // Only treat the argument as a path when it looks like one; otherwise a
    // typo'd built-in name would surface as a confusing file-not-found.
    if (std::filesystem::exists(path)) {
        return loadPreset(path);
    }
    return Error{ErrorCode::UnsupportedFormat, "unknown preset '" + nameOrPath +
                                                   "' (built-ins: " + joinBuiltinNames() +
                                                   "; or pass a path to a preset file)"};
}

std::string presetMapFileName(const ExportPreset& preset, const PresetMapEntry& entry,
                              std::string_view basename) {
    std::string name = preset.namingPattern;
    const std::string suffix = entry.suffix.empty() ? presetMapName(entry.map) : entry.suffix;
    replaceAll(name, "{basename}", basename);
    replaceAll(name, "{map}", suffix);
    replaceAll(name, "{preset}", preset.name);
    replaceAll(name, "{ext}", preset.textureFormat);
    // Containment is decided on the EXPANDED name, not per token: every token
    // ({basename}, {map}, {preset}, {ext}) substitutes text from a preset file
    // or a caller, so gating them one by one leaves the next token added here
    // open. An escaping expansion yields no name at all -- callers treat the
    // empty string as a refusal rather than joining it to their directory.
    if (escapesOutputDirectory(name)) {
        return {};
    }
    return name;
}

}  // namespace cyber::io
