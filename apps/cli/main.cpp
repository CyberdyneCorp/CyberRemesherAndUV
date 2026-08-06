// cyberremesh — headless batch remeshing (cli-headless spec).
//
// Exit codes (spec: "Exit codes reflect outcomes"):
//   0 success | 2 argument error | 3 input load failure | 4 pipeline failure
//   or empty result | 5 partial success (failed islands) | 6 output/report
//   write failure | 130 cancelled (SIGINT).
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define CYBER_ISATTY _isatty
#define CYBER_FILENO _fileno
#else
#include <unistd.h>
#define CYBER_ISATTY isatty
#define CYBER_FILENO fileno
#endif

#include <json.hpp>

#include "cyber/accel/backend.hpp"
#include "cyber/core/export_preset.hpp"
#include "cyber/core/io.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/version.hpp"
#include "cyber/handoff/handoff.hpp"
#include "cyber/quadrangulate/field_quadrangulator.hpp"
#include "cyber/quadrangulate/position_field.hpp"
#include "cyber/quadrangulate/quadcover_extractor.hpp"

#ifdef CYBER_CLI_HAVE_PRESETS
#include "cyber/exportbundle/bundle.hpp"
#endif

namespace {

using cyber::CancelToken;
namespace remesh = cyber::remesh;

constexpr int kExitOk = 0;
constexpr int kExitArgs = 2;
constexpr int kExitLoad = 3;
constexpr int kExitPipeline = 4;
constexpr int kExitPartial = 5;
constexpr int kExitWrite = 6;
constexpr int kExitCancelled = 130;

// SIGINT -> cooperative cancellation.
std::atomic<bool> g_interrupted{false};
void onSigint(int) { g_interrupted.store(true); }

struct CliOptions {
    std::string input;
    std::string target;    // sculpt handoff (--target); "-" reads stdin
    std::string bakeMaps;  // --bake csv override of the preset's map set
    std::string output;
    std::string report;
    std::string guides;                     // guidance sidecar (--guides); empty = no guidance
    std::string quadMethod = "quad-cover";  // roadmap default (2026-07-22)
    std::string preset;                     // empty = mesh-only export (the historical behaviour)
    float cageDistance = 0.0f;              // 0 = derive from the input's bounds
    int textureSize = 0;                    // 0 = the preset's own resolution
    int aoSamples = 0;                      // 0 = the bake default
    remesh::Parameters params;
    bool verbose = false;
    bool quiet = false;
};

void printUsage() {
    std::fprintf(stderr,
                 "usage: cyberremesh --input <mesh> --output <mesh> [options]\n"
                 "       cyberremesh --target <handoff|-> --output <mesh> [options]\n"
                 "  --target <path|->        sculpt handoff as the Target (PLY/GLB\n"
                 "                           profile, docs/sculpt-handoff-format.md);\n"
                 "                           '-' reads the handoff from stdin.\n"
                 "                           Mutually exclusive with --input\n"
                 "  --bake <csv>             maps to bake, overriding the preset's set\n"
                 "                           (normal,ao,curvature,cavity,displacement,\n"
                 "                           color,position). Implies --preset\n"
                 "                           gltf-generic when no preset is named\n"
                 "  --target-quads <int>     target quad count (default 50000)\n"
                 "  --edge-scale <float>     density scale (default 1.0)\n"
                 "  --sharp-edge <deg>       feature angle (default 90)\n"
                 "  --smooth-normal <deg>    smooth projection angle (default 0)\n"
                 "  --adaptivity <float>     0..1 curvature adaptivity (default 1)\n"
                 "  --pure-quads             quads-only output\n"
                 "  --quad-method <m>        quad-cover | field-aligned | instant-meshes |\n"
                 "                           integer | greedy (default quad-cover; falls\n"
                 "                           back to field-aligned without a UV solver)\n"
                 "  --hole-fill <int>        max hole boundary to fill (default 64)\n"
                 "  --patch-policy <p>       keep-largest | keep-all | min-faces:<N>\n"
                 "  --report <path.json>     machine-readable run report\n"
                 "  --guides <path.json>     flow-guide / painted-density sidecar\n"
                 "  --preset <name|path>     export a per-DCC bundle (mesh + baked maps)\n"
                 "  --cage <float>           bake cage distance (default: 1%% of the\n"
                 "                           input's bounding-box diagonal)\n"
                 "  --texture-size <int>     override the preset's map resolution\n"
                 "  --ao-samples <int>       hemisphere rays per texel (default 64)\n"
                 "  --list-presets           print built-in export presets and exit\n"
                 "  --verbose | --quiet      diagnostic detail / errors only\n"
                 "  --list-backends          print compute backends and exit\n"
                 "  --version                print version and exit\n");
}

template <typename T>
std::optional<T> parseNumber(const std::string& text) {
    T value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Returns exit code (kExitOk to continue) and fills options.
int parseArgs(int argc, char** argv, CliOptions& options, bool& exitEarly) {
    exitEarly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", flag);
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        auto numeric = [&](const char* flag, auto& target) -> bool {
            const auto text = next(flag);
            if (!text) {
                return false;
            }
            const auto value = parseNumber<std::decay_t<decltype(target)>>(*text);
            if (!value) {
                // Non-numeric values are rejected, never silently zero
                // (AutoRemesher converted "abc" to 0 silently — spec'd away).
                std::fprintf(stderr, "error: %s expects a number, got '%s'\n", flag, text->c_str());
                return false;
            }
            target = *value;
            return true;
        };

        if (arg == "--version") {
            std::printf("cyberremesh %.*s\n", static_cast<int>(cyber::version().size()),
                        cyber::version().data());
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--list-backends") {
            for (const auto& backend : cyber::accel::availableBackends()) {
                std::printf("%s\n", backend->deviceName().c_str());
            }
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--list-presets") {
            for (const std::string& name : cyber::io::builtinPresetNames()) {
                std::printf("%s\n", name.c_str());
            }
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage();
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--input" || arg == "-i") {
            const auto v = next("--input");
            if (!v) {
                return kExitArgs;
            }
            options.input = *v;
        } else if (arg == "--target") {
            const auto v = next("--target");
            if (!v) {
                return kExitArgs;
            }
            options.target = *v;
        } else if (arg == "--bake") {
            const auto v = next("--bake");
            if (!v) {
                return kExitArgs;
            }
            options.bakeMaps = *v;
        } else if (arg == "--output" || arg == "-o") {
            const auto v = next("--output");
            if (!v) {
                return kExitArgs;
            }
            options.output = *v;
        } else if (arg == "--preset") {
            const auto v = next("--preset");
            if (!v) {
                return kExitArgs;
            }
            options.preset = *v;
        } else if (arg == "--cage") {
            if (!numeric("--cage", options.cageDistance)) {
                return kExitArgs;
            }
        } else if (arg == "--texture-size") {
            if (!numeric("--texture-size", options.textureSize)) {
                return kExitArgs;
            }
        } else if (arg == "--ao-samples") {
            if (!numeric("--ao-samples", options.aoSamples)) {
                return kExitArgs;
            }
        } else if (arg == "--guides") {
            const auto v = next("--guides");
            if (!v) {
                return kExitArgs;
            }
            options.guides = *v;
        } else if (arg == "--report") {
            const auto v = next("--report");
            if (!v) {
                return kExitArgs;
            }
            options.report = *v;
        } else if (arg == "--target-quads") {
            if (!numeric("--target-quads", options.params.targetQuadCount)) {
                return kExitArgs;
            }
        } else if (arg == "--edge-scale") {
            if (!numeric("--edge-scale", options.params.edgeScale)) {
                return kExitArgs;
            }
        } else if (arg == "--sharp-edge") {
            if (!numeric("--sharp-edge", options.params.sharpEdgeDegrees)) {
                return kExitArgs;
            }
        } else if (arg == "--smooth-normal") {
            if (!numeric("--smooth-normal", options.params.smoothNormalDegrees)) {
                return kExitArgs;
            }
        } else if (arg == "--adaptivity") {
            if (!numeric("--adaptivity", options.params.adaptivity)) {
                return kExitArgs;
            }
        } else if (arg == "--hole-fill") {
            if (!numeric("--hole-fill", options.params.holeFillMaxBoundary)) {
                return kExitArgs;
            }
        } else if (arg == "--pure-quads") {
            options.params.pureQuads = true;
        } else if (arg == "--quad-method") {
            const auto v = next("--quad-method");
            if (!v) {
                return kExitArgs;
            }
            if (*v != "quad-cover" && *v != "field-aligned" && *v != "instant-meshes" &&
                *v != "integer" && *v != "greedy") {
                std::fprintf(stderr, "error: unknown --quad-method '%s'\n", v->c_str());
                return kExitArgs;
            }
            options.quadMethod = *v;
        } else if (arg == "--patch-policy") {
            const auto v = next("--patch-policy");
            if (!v) {
                return kExitArgs;
            }
            if (*v == "keep-largest") {
                options.params.smallPatchPolicy = remesh::SmallPatchPolicy::KeepLargest;
            } else if (*v == "keep-all") {
                options.params.smallPatchPolicy = remesh::SmallPatchPolicy::KeepAll;
            } else if (v->rfind("min-faces:", 0) == 0) {
                const auto n = parseNumber<int>(v->substr(10));
                if (!n) {
                    std::fprintf(stderr, "error: --patch-policy min-faces:<N> needs a number\n");
                    return kExitArgs;
                }
                options.params.smallPatchPolicy = remesh::SmallPatchPolicy::MinFaces;
                options.params.smallPatchMinFaces = *n;
            } else {
                std::fprintf(stderr, "error: unknown patch policy '%s'\n", v->c_str());
                return kExitArgs;
            }
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            printUsage();
            return kExitArgs;
        }
    }
    // --input and --target are two names for the same slot; taking both would
    // leave the engine silently choosing one, so it is an argument error.
    if (!options.input.empty() && !options.target.empty()) {
        std::fprintf(stderr, "error: --input and --target are mutually exclusive\n");
        return kExitArgs;
    }
    if ((options.input.empty() && options.target.empty()) || options.output.empty()) {
        std::fprintf(stderr, "error: --input (or --target) and --output are required\n");
        printUsage();
        return kExitArgs;
    }
    // --bake names maps, which only a preset run can produce. Defaulting to the
    // format-neutral bundle is the least surprising way to honor the flag on
    // its own rather than rejecting it.
    if (!options.bakeMaps.empty() && options.preset.empty()) {
        options.preset = "gltf-generic";
    }
    return kExitOk;
}

// --bake <csv> -> the preset's map list. Unknown names are an argument error
// naming the offender: silently dropping a requested map would ship an asset
// missing exactly the texture the user asked for.
//
// Guarded to match its only call site: without the export-bundle module there
// is nothing to bake, and an unconditional definition is an unused function,
// which this tree compiles as an error.
#ifdef CYBER_CLI_HAVE_PRESETS
bool applyBakeMapOverride(const std::string& csv, cyber::io::ExportPreset& preset,
                          std::string& error) {
    std::vector<cyber::io::PresetMapEntry> maps;
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t comma = csv.find(',', start);
        const std::size_t end = comma == std::string::npos ? csv.size() : comma;
        std::string name = csv.substr(start, end - start);
        start = end + 1;
        while (!name.empty() && name.front() == ' ') {
            name.erase(name.begin());
        }
        while (!name.empty() && name.back() == ' ') {
            name.pop_back();
        }
        if (name.empty()) {
            continue;
        }
        const auto map = cyber::io::presetMapFromName(name);
        if (!map) {
            error = "unknown --bake map '" + name + "'";
            return false;
        }
        cyber::io::PresetMapEntry entry;
        entry.map = *map;
        // Color carries appearance, so it keeps the sRGB encoding the built-in
        // presets give it; every other map carries data and stays linear.
        entry.colorSpace = *map == cyber::io::PresetMap::Color ? cyber::io::ColorSpace::Srgb
                                                               : cyber::io::ColorSpace::Linear;
        entry.suffix = cyber::io::presetMapName(*map);
        maps.push_back(std::move(entry));
    }
    if (maps.empty()) {
        error = "--bake needs at least one map name";
        return false;
    }
    preset.maps = std::move(maps);
    return true;
}
#endif  // CYBER_CLI_HAVE_PRESETS

// What a --target run ingested, for the report.
struct HandoffOutcome {
    bool active = false;
    std::string source;
    cyber::handoff::Version version;
    std::string producer;
    std::size_t vertexCount = 0;
    std::size_t faceCount = 0;
    bool hasVertexColors = false;
    bool hasVertexNormals = false;
    bool hasMaterialMix = false;
};

void addHandoffToReport(nlohmann::json& report, const HandoffOutcome& outcome) {
    if (!outcome.active) {
        return;
    }
    report["handoff"] = {
        {"source", outcome.source},
        {"version", {{"major", outcome.version.major}, {"minor", outcome.version.minor}}},
        {"supportedVersion",
         {{"major", cyber::handoff::kVersionMajor}, {"minor", cyber::handoff::kVersionMinor}}},
        {"producer", outcome.producer},
        {"vertices", outcome.vertexCount},
        {"faces", outcome.faceCount},
        {"hasVertexColors", outcome.hasVertexColors},
        {"hasVertexNormals", outcome.hasVertexNormals},
        {"hasMaterialMix", outcome.hasMaterialMix},
        // No field evaluator is reachable from the CLI yet (the evaluator is a
        // C++/C-ABI interface, and this build links no volumetric engine), so
        // nothing here was field-sampled. Recorded explicitly rather than
        // omitted so a consumer can tell "none" from "not reported".
        {"fieldSampledMaps", nlohmann::json::array()},
    };
}

// What a --preset run produced, in a form the report can serialise whether or
// not this build has the bundle module compiled in.
struct PresetOutcome {
    bool active = false;
    cyber::io::ExportPreset preset;
    struct File {
        std::string path;
        std::string kind;
        std::string colorSpace;
        int width = 0;
        int height = 0;
    };
    std::vector<File> files;
    bool unwrapped = false;
    int chartCount = 0;
};

const char* greenName(cyber::io::GreenChannel green) {
    return green == cyber::io::GreenChannel::MinusY ? "-Y" : "+Y";
}

void addPresetToReport(nlohmann::json& report, const PresetOutcome& outcome) {
    if (!outcome.active) {
        return;
    }
    const cyber::io::ExportPreset& preset = outcome.preset;
    nlohmann::json maps = nlohmann::json::array();
    for (const auto& entry : preset.maps) {
        maps.push_back({{"map", cyber::io::presetMapName(entry.map)},
                        {"colorSpace", cyber::io::colorSpaceName(entry.colorSpace)},
                        {"suffix", entry.suffix}});
    }
    report["preset"] = {
        {"name", preset.name},
        {"schemaVersion", preset.schemaVersion},
        {"meshFormat", preset.meshFormat},
        {"textureFormat", preset.textureFormat},
        {"namingPattern", preset.namingPattern},
        {"normalGreen", greenName(preset.normalGreen)},
        {"resolution", preset.resolution},
        {"units", preset.units},
        {"upAxis", preset.upAxis},
        {"maps", maps},
        {"unwrapped", outcome.unwrapped},
        {"chartCount", outcome.chartCount},
    };
    report["outputs"] = nlohmann::json::array();
    for (const PresetOutcome::File& file : outcome.files) {
        nlohmann::json entry = {{"path", file.path}, {"kind", file.kind}};
        if (file.width > 0) {
            entry["colorSpace"] = file.colorSpace;
            entry["width"] = file.width;
            entry["height"] = file.height;
        }
        report["outputs"].push_back(entry);
    }
}

const char* statusName(remesh::RunStatus status) {
    switch (status) {
        case remesh::RunStatus::Success:
            return "success";
        case remesh::RunStatus::Partial:
            return "partial";
        case remesh::RunStatus::Cancelled:
            return "cancelled";
        case remesh::RunStatus::Error:
            return "error";
    }
    return "unknown";
}

// ---- guidance sidecar (--guides) ---------------------------------------
//
// {"version": 1,
//  "guides": [{"points": [[x,y,z], ...], "strength": 1.0, "radius": 0.15}],
//  "density": {"perVertex": [...]}}      (or "perFace")
//
// Anything wrong with the file is exit 2 naming the file and the offending
// guide index. A malformed sidecar is NEVER a silent skip: the whole point of
// the feature is that supplied guidance is honored loudly or rejected loudly.
//
// Every value is type-checked before it is read: nlohmann THROWS on a type
// mismatch (a string where a number belongs), and an escaping throw would
// abort the process instead of producing the exit-2 argument error a bad file
// deserves.

// Optional numeric field. Absent leaves `out` at the caller's default.
bool readNumberField(const nlohmann::json& obj, const char* key, const std::string& where,
                     float& out, std::string& error) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    if (!it->is_number()) {
        error = where + ": \"" + std::string(key) + "\" must be a number";
        return false;
    }
    out = it->get<float>();
    return true;
}

bool readNumberArray(const nlohmann::json& node, const std::string& where, std::vector<float>& out,
                     std::string& error) {
    if (!node.is_array()) {
        error = where + " must be an array of numbers";
        return false;
    }
    out.clear();
    out.reserve(node.size());
    for (std::size_t i = 0; i < node.size(); ++i) {
        if (!node[i].is_number()) {
            error = where + "[" + std::to_string(i) + "] must be a number";
            return false;
        }
        out.push_back(node[i].get<float>());
    }
    return true;
}

bool parseGuide(const nlohmann::json& g, const std::string& where, remesh::Guidance& out,
                std::string& error) {
    if (!g.is_object() || !g.contains("points") || !g["points"].is_array()) {
        error = where + ": missing a \"points\" array";
        return false;
    }
    remesh::FlowGuide guide;
    if (!readNumberField(g, "strength", where, guide.strength, error) ||
        !readNumberField(g, "radius", where, guide.radius, error)) {
        return false;
    }
    for (const auto& p : g["points"]) {
        std::vector<float> xyz;
        if (!p.is_array() || p.size() != 3 || !readNumberArray(p, where + " point", xyz, error)) {
            error = where + ": each point must be [x, y, z] of numbers";
            return false;
        }
        guide.points.push_back(cyber::Vec3{xyz[0], xyz[1], xyz[2]});
    }
    out.guides.push_back(std::move(guide));
    return true;
}

bool parseGuidanceDoc(const nlohmann::json& doc, const std::string& path, remesh::Guidance& out,
                      std::string& error) {
    if (!doc.is_object()) {
        error = "guides file '" + path + "' must contain a JSON object";
        return false;
    }
    const std::string file = "guides file '" + path + "'";
    int version = 1;
    if (const auto it = doc.find("version"); it != doc.end()) {
        if (!it->is_number_integer()) {
            error = file + ": \"version\" must be an integer";
            return false;
        }
        version = it->get<int>();
    }
    if (version != 1) {
        error = file + ": unsupported version " + std::to_string(version);
        return false;
    }
    if (doc.contains("guides")) {
        if (!doc["guides"].is_array()) {
            error = file + ": \"guides\" must be an array";
            return false;
        }
        for (std::size_t i = 0; i < doc["guides"].size(); ++i) {
            if (!parseGuide(doc["guides"][i], file + " guide " + std::to_string(i), out, error)) {
                return false;
            }
        }
    }
    if (doc.contains("density")) {
        const nlohmann::json& d = doc["density"];
        if (!d.is_object()) {
            error = file + ": \"density\" must be an object";
            return false;
        }
        if (d.contains("perVertex") &&
            !readNumberArray(d["perVertex"], file + ": \"density.perVertex\"",
                             out.density.vertexValues, error)) {
            return false;
        }
        if (d.contains("perFace") && !readNumberArray(d["perFace"], file + ": \"density.perFace\"",
                                                      out.density.faceValues, error)) {
            return false;
        }
    }
    return true;
}

bool loadGuidanceSidecar(const std::string& path, remesh::Guidance& out, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot read guides file '" + path + "'";
        return false;
    }
    nlohmann::json doc;
    try {
        file >> doc;
        // The typed readers above cover every documented field; the catch is
        // the backstop that keeps an unforeseen nlohmann throw an exit-2
        // argument error rather than a std::terminate.
        return parseGuidanceDoc(doc, path, out, error);
    } catch (const std::exception& e) {
        error = "guides file '" + path + "' is not valid JSON: " + e.what();
        return false;
    }
}

// Post-clamp guidance block for the machine-readable report (remeshing-
// parameters spec: "The effective post-clamp values SHALL appear in the
// machine-readable report").
void addGuidanceToReport(nlohmann::json& report, const remesh::ValidatedGuidance& guidance,
                         const remesh::PipelineResult& result) {
    nlohmann::json block;
    block["guides"] = nlohmann::json::array();
    for (const auto& g : guidance.guidance.guides) {
        block["guides"].push_back(
            {{"pointCount", g.points.size()}, {"strength", g.strength}, {"radius", g.radius}});
    }
    const auto& density = guidance.guidance.density;
    const bool haveDensity = !density.empty();
    block["density"] = {
        {"present", haveDensity},
        {"kind", density.vertexValues.empty() ? "perFace" : "perVertex"},
        {"count",
         density.vertexValues.empty() ? density.faceValues.size() : density.vertexValues.size()},
        {"clampRange", {remesh::kDensityMin, remesh::kDensityMax}},
    };
    block["issues"] = nlohmann::json::array();
    for (const auto& issue : guidance.issues) {
        block["issues"].push_back(
            {{"parameter", issue.parameter}, {"message", issue.message}, {"fatal", issue.fatal}});
    }
    block["islands"] = nlohmann::json::array();
    for (const auto& row : result.islandGuidance) {
        block["islands"].push_back({{"island", row.islandIndex},
                                    {"guidesInRange", row.guidesInRange},
                                    {"guidesHonored", row.guidesHonored},
                                    {"densityHonored", row.densityHonored},
                                    {"reason", row.reason}});
    }
    report["guidance"] = block;
}

// Writing the report is a hard requirement when requested: an unwritable
// path is exit 6, never silently skipped (spec).
int writeReport(const CliOptions& options, const remesh::PipelineResult& result,
                double elapsedSeconds, const PresetOutcome& presetOutcome,
                const remesh::ValidatedGuidance& guidance, const HandoffOutcome& handoffOutcome) {
    nlohmann::json report;
    report["tool"] = "cyberremesh";
    report["version"] = std::string(cyber::version());
    report["input"] = options.input.empty() ? options.target : options.input;
    report["output"] = options.output;
    report["status"] = statusName(result.status);
    report["elapsedSeconds"] = elapsedSeconds;
    report["parameters"] = {
        {"targetQuadCount", options.params.targetQuadCount},
        {"edgeScale", options.params.edgeScale},
        {"sharpEdgeDegrees", options.params.sharpEdgeDegrees},
        {"smoothNormalDegrees", options.params.smoothNormalDegrees},
        {"adaptivity", options.params.adaptivity},
        {"pureQuads", options.params.pureQuads},
        {"holeFillMaxBoundary", options.params.holeFillMaxBoundary},
    };
    report["statistics"] = {
        {"vertices", result.stats.vertexCount},
        {"quads", result.stats.quadCount},
        {"triangles", result.stats.triangleCount},
        {"otherPolygons", result.stats.otherPolygonCount},
        {"islands", result.stats.islandCount},
        {"islandsFailed", result.stats.islandsFailed},
        {"targetEdgeLength", result.stats.targetEdgeLength},
    };
    report["warnings"] = nlohmann::json::array();
    for (const auto& issue : result.parameterIssues) {
        report["warnings"].push_back(issue.message);
    }
    report["failedIslands"] = nlohmann::json::array();
    for (const auto& diag : result.failedIslands) {
        report["failedIslands"].push_back({{"island", diag.islandIndex},
                                           {"inputFaces", diag.inputFaces},
                                           {"stage", diag.stage},
                                           {"reason", diag.reason}});
    }
    addPresetToReport(report, presetOutcome);
    addHandoffToReport(report, handoffOutcome);
    if (!options.guides.empty()) {
        addGuidanceToReport(report, guidance, result);
    }

    std::ofstream file(options.report, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "error: cannot write report to '%s'\n", options.report.c_str());
        return kExitWrite;
    }
    file << report.dump(2) << '\n';
    if (!file.flush()) {
        std::fprintf(stderr, "error: write to '%s' failed\n", options.report.c_str());
        return kExitWrite;
    }
    return kExitOk;
}

int runCli(int argc, char** argv) {
    CliOptions options;
    bool exitEarly = false;
    const int argStatus = parseArgs(argc, argv, options, exitEarly);
    if (argStatus != kExitOk || exitEarly) {
        return argStatus;
    }
    std::signal(SIGINT, onSigint);

    // Resolve the preset BEFORE the remesh: an unknown preset is an argument
    // error, and making the user wait through a full solve to learn they typo'd
    // a name would be the opposite of loud.
    PresetOutcome presetOutcome;
    if (!options.preset.empty()) {
#ifndef CYBER_CLI_HAVE_PRESETS
        std::fprintf(stderr,
                     "error: this build has no export-preset support "
                     "(configure with -DCYBER_BUILD_UV=ON)\n");
        return kExitArgs;
#else
        const auto resolved = cyber::io::resolvePreset(options.preset);
        if (!resolved.ok()) {
            std::fprintf(stderr, "error: %s\n", resolved.error().message.c_str());
            return kExitArgs;
        }
        presetOutcome.active = true;
        presetOutcome.preset = resolved.value();
        if (options.textureSize > 0) {
            presetOutcome.preset.resolution = options.textureSize;
        }
        if (!options.bakeMaps.empty()) {
            std::string bakeError;
            if (!applyBakeMapOverride(options.bakeMaps, presetOutcome.preset, bakeError)) {
                std::fprintf(stderr, "error: %s\n", bakeError.c_str());
                return kExitArgs;
            }
        }
#endif
    }

    // The Target: either a plain mesh (--input) or a versioned sculpt handoff
    // (--target). A rejected handoff — including an incompatible version — is
    // exit 3 and writes nothing.
    HandoffOutcome handoffOutcome;
    cyber::io::ImportedMesh source;
    if (!options.target.empty()) {
        const bool fromStdin = options.target == "-";
#ifdef _WIN32
        if (fromStdin) {
            _setmode(_fileno(stdin), _O_BINARY);  // binary_little_endian PLY on stdin
        }
#endif
        auto handoff = fromStdin ? cyber::handoff::readStream(std::cin, "<stdin>")
                                 : cyber::handoff::readFile(options.target);
        if (!handoff.ok()) {
            std::fprintf(stderr, "error: %s\n", handoff.error().message.c_str());
            return kExitLoad;
        }
        cyber::handoff::Handoff& h = handoff.value();
        handoffOutcome.active = true;
        handoffOutcome.source = fromStdin ? "<stdin>" : options.target;
        handoffOutcome.version = h.version;
        handoffOutcome.producer = h.producer;
        handoffOutcome.vertexCount = h.mesh.vertexCount();
        handoffOutcome.faceCount = h.mesh.faceCount();
        handoffOutcome.hasVertexColors = h.hasVertexColors();
        handoffOutcome.hasVertexNormals = h.hasVertexNormals();
        handoffOutcome.hasMaterialMix = h.hasMaterialMix();
        source.mesh = std::move(h.mesh);
        source.boundsMin = h.boundsMin;
        source.boundsMax = h.boundsMax;
        source.warnings = std::move(h.warnings);
    } else {
        auto imported = cyber::io::importMesh(options.input);
        if (!imported.ok()) {
            std::fprintf(stderr, "error: %s\n", imported.error().message.c_str());
            return kExitLoad;
        }
        source = std::move(imported.value());
    }
    if (!options.quiet) {
        for (const auto& warning : source.warnings) {
            std::fprintf(stderr, "warning: %s\n", warning.c_str());
        }
    }

    const CancelToken cancel;
    const bool showProgress = !options.quiet && CYBER_ISATTY(CYBER_FILENO(stdout)) != 0;
    cyber::ProgressSink sink([&](float p, std::string_view stage) {
        if (g_interrupted.load()) {
            cancel.requestCancel();
        }
        if (showProgress) {
            std::printf("\r%3d%% %.*s   ", static_cast<int>(p * 100.0f),
                        static_cast<int>(stage.size()), stage.data());
            std::fflush(stdout);
        }
    });

    // Guidance sidecar: parsed and validated BEFORE the solve. A missing,
    // unreadable, malformed or fatally-invalid sidecar is exit 2 naming the
    // file and the offending guide — never a silent skip.
    remesh::Guidance rawGuidance;
    remesh::ValidatedGuidance validatedGuidance;
    if (!options.guides.empty()) {
        std::string guidesError;
        if (!loadGuidanceSidecar(options.guides, rawGuidance, guidesError)) {
            std::fprintf(stderr, "error: %s\n", guidesError.c_str());
            return kExitArgs;
        }
        validatedGuidance = remesh::validateGuidance(rawGuidance, source.mesh.vertexCount(),
                                                     source.mesh.faceCount());
        if (!validatedGuidance.ok()) {
            for (const auto& issue : validatedGuidance.issues) {
                if (issue.fatal) {
                    std::fprintf(stderr, "error: guides file '%s': %s\n", options.guides.c_str(),
                                 issue.message.c_str());
                }
            }
            return kExitArgs;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    // Default method is quad-cover (docs/ROADMAP.md, 2026-07-22) — the CLI
    // previously hardcoded field-aligned, silently diverging from the
    // documented default. Falls back to field-aligned when no seamless-UV
    // solver is available so solver-less builds still produce output.
    std::string method = options.quadMethod;
    if (method == "quad-cover" && !remesh::quadCoverAvailable()) {
        if (!options.quiet) {
            std::fprintf(stderr,
                         "warning: no seamless-UV solver in this build; "
                         "using --quad-method field-aligned\n");
        }
        method = "field-aligned";
    }
    const float adaptivity = options.params.adaptivity;
    const int holeFill = options.params.holeFillMaxBoundary;
    const float sharpDegrees = options.params.sharpEdgeDegrees;
    const auto makeQuadrangulator = [&method, adaptivity, holeFill,
                                     sharpDegrees]() -> std::unique_ptr<remesh::IQuadrangulator> {
        if (method == "quad-cover") {
            // --sharp-edge (default 90) binds sharp edges into the native
            // solve: feature seams + integer-pinned crease isolines
            // (docs/ROADMAP.md 2026-08-01 — box_sharp recall 0.04 -> 1.00 at
            // 8 singularities, organics neutral-to-better). Revert to the
            // historical knife-edge-only binding with --sharp-edge 40 or
            // CYBER_QC_FEATURE_DEG=40.
            return remesh::makeQuadCoverQuadrangulator(40, adaptivity, holeFill, sharpDegrees);
        }
        if (method == "instant-meshes") {
            return remesh::makeInstantMeshesQuadrangulator();
        }
        if (method == "integer") {
            return remesh::makeIntegerQuadrangulator();
        }
        if (method == "greedy") {
            return remesh::makeGreedyPairingQuadrangulator();
        }
        return remesh::makeFieldAlignedQuadrangulator();
    };
    // Field-aligned fallback for quad-cover (pipeline contract: when quad-cover declines
    // an island — no usable seamless UV, divergence guard, extraction coverage gate — the
    // island is recovered via the normal isotropic remesh + field-aligned quadrangulator
    // instead of failing outright). Only wired for the quad-cover method; other methods
    // keep their single-quadrangulator behavior.
    const remesh::QuadrangulatorFactory fallbackFactory =
        method == "quad-cover" ? remesh::QuadrangulatorFactory(
                                     []() { return remesh::makeFieldAlignedQuadrangulator(); })
                               : remesh::QuadrangulatorFactory{};
    const remesh::PipelineResult result =
        remesh::remesh(source.mesh, options.params, &sink, &cancel, makeQuadrangulator,
                       fallbackFactory, rawGuidance.empty() ? nullptr : &rawGuidance);
    if (showProgress) {
        std::printf("\r");
    }

    if (!options.quiet) {
        for (const auto& issue : result.parameterIssues) {
            std::fprintf(stderr, "warning: %s\n", issue.message.c_str());
        }
        // Unhonored guidance is a warning the user must see, not a report-only
        // detail: an island whose backend cannot follow the strokes looks
        // identical to one that did unless we say so.
        for (const auto& row : result.islandGuidance) {
            if (row.reason.empty()) {
                continue;
            }
            std::fprintf(
                stderr, "warning: island %zu: %zu guide(s) in range, guides %s, density %s (%s)\n",
                row.islandIndex, row.guidesInRange, row.guidesHonored ? "honored" : "NOT honored",
                row.densityHonored ? "honored" : "NOT honored", row.reason.c_str());
        }
        for (const auto& diag : result.failedIslands) {
            std::fprintf(stderr, "warning: island %zu failed at %s: %s\n", diag.islandIndex,
                         diag.stage.c_str(), diag.reason.c_str());
        }
    }

    if (result.status == remesh::RunStatus::Cancelled) {
        std::fprintf(stderr, "cancelled\n");
        return kExitCancelled;
    }
    if (result.status == remesh::RunStatus::Error) {
        std::fprintf(stderr, "error: %s\n", result.error.c_str());
        return kExitPipeline;
    }

#ifdef CYBER_CLI_HAVE_PRESETS
    if (presetOutcome.active) {
        cyber::exportbundle::BundleParams bundleParams;
        bundleParams.preset = presetOutcome.preset;
        bundleParams.meshPath = options.output;
        // The cage has to scale with the model: a fixed default silently misses
        // the Target on anything not roughly unit-sized. 1% of the input's
        // bounding-box diagonal reaches the detail without punching through to
        // the far side on the corpus meshes.
        const cyber::Vec3 extent = source.boundsMax - source.boundsMin;
        bundleParams.cageDistance =
            options.cageDistance > 0.0f ? options.cageDistance : 0.01f * cyber::length(extent);
        if (options.aoSamples > 0) {
            bundleParams.aoSamples = options.aoSamples;
        }
        cyber::Mesh low = result.mesh;
        const cyber::exportbundle::BundleResult bundle =
            cyber::exportbundle::writeBundle(low, source.mesh, bundleParams, &sink, &cancel);
        if (!options.quiet) {
            for (const std::string& warning : bundle.warnings) {
                std::fprintf(stderr, "warning: %s\n", warning.c_str());
            }
        }
        if (bundle.cancelled) {
            std::fprintf(stderr, "cancelled\n");
            return kExitCancelled;
        }
        if (!bundle.ok) {
            std::fprintf(stderr, "error: %s\n", bundle.error.c_str());
            return kExitWrite;
        }
        presetOutcome.unwrapped = bundle.unwrapped;
        presetOutcome.chartCount = bundle.chartCount;
        for (const auto& file : bundle.files) {
            presetOutcome.files.push_back(
                {file.path, file.kind, file.colorSpace, file.width, file.height});
        }
    }
#endif
    if (!presetOutcome.active) {
        const auto exportStatus = cyber::io::exportMesh(result.mesh, options.output);
        if (!exportStatus.ok()) {
            std::fprintf(stderr, "error: %s\n", exportStatus.error().message.c_str());
            return kExitWrite;
        }
    }

    // Measured here, not right after the solve: with --preset the bake and
    // write dominate, and a time that excluded them would be misleading.
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!options.report.empty()) {
        const int reportStatus =
            writeReport(options, result, elapsed, presetOutcome, validatedGuidance, handoffOutcome);
        if (reportStatus != kExitOk) {
            return reportStatus;
        }
    }

    if (!options.quiet) {
        std::printf("=== cyberremesh report ===\n");
        std::printf("input:     %s\n",
                    options.input.empty() ? handoffOutcome.source.c_str() : options.input.c_str());
        std::printf("output:    %s\n", options.output.c_str());
        std::printf("status:    %s\n", statusName(result.status));
        std::printf("vertices:  %zu\n", result.stats.vertexCount);
        std::printf("quads:     %zu\n", result.stats.quadCount);
        std::printf("non-quads: %zu\n",
                    result.stats.triangleCount + result.stats.otherPolygonCount);
        std::printf("islands:   %zu (%zu failed)\n", result.stats.islandCount,
                    result.stats.islandsFailed);
        if (presetOutcome.active) {
            std::printf("preset:    %s (schema %d)\n", presetOutcome.preset.name.c_str(),
                        presetOutcome.preset.schemaVersion);
            for (const PresetOutcome::File& file : presetOutcome.files) {
                std::printf("  %-12s %s\n", file.kind.c_str(), file.path.c_str());
            }
        }
        std::printf("time:      %.2fs\n", elapsed);
    }
    return result.status == remesh::RunStatus::Partial ? kExitPartial : kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    // Last-resort guard: a CLI must report a failure, never abort. Without it
    // any escaping exception (a JSON reader, an allocation, a decoder) reaches
    // std::terminate and the user gets SIGABRT instead of a diagnosis.
    try {
        return runCli(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: unexpected failure: %s\n", e.what());
        return kExitPipeline;
    }
}
