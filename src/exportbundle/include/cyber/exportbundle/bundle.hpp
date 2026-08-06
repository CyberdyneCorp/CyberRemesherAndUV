#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cyber/core/export_preset.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

// Preset-driven export packaging (mesh-io spec, "Named export presets"). Turns
// a preset plus a low/high mesh pair into the files a target DCC expects: the
// mesh, then one baked map per preset entry, named by the preset's pattern and
// written in its color-space and normal conventions.
//
// This sits above core (which owns the preset DATA and knows nothing of baking)
// and below the CLI (which owns flags and reporting).
namespace cyber::exportbundle {

struct BundleParams {
    io::ExportPreset preset;
    // Where the mesh is written. Its extension wins over preset.meshFormat --
    // an explicit output path is the user speaking last -- and a mismatch is
    // reported as a warning rather than silently resolved.
    std::filesystem::path meshPath;
    // Substituted for {basename} in the preset's naming pattern. Empty means
    // "the mesh path's stem".
    std::string basename;
    // Projection cage for every bake, in model units.
    float cageDistance = 0.1f;
    int aoSamples = 64;
    float aoRadius = 1.0f;
};

struct BundleFile {
    std::string path;
    std::string kind;        // "mesh", or the preset map name ("normal", "ao", ...)
    std::string colorSpace;  // "linear" | "srgb" — the encoding actually written
    int width = 0;           // 0 for the mesh
    int height = 0;
};

struct BundleResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::vector<BundleFile> files;
    std::vector<std::string> warnings;
    // Set when the low-poly carried no UVs and the bundle unwrapped it.
    bool unwrapped = false;
    int chartCount = 0;
    float maxAngleDistortion = 0.0f;
};

// Writes the bundle. `low` is modified in place when it needs UVs: baking is
// impossible without them, and requiring the caller to pre-unwrap would make
// `--preset` useless on a freshly remeshed mesh.
[[nodiscard]] BundleResult writeBundle(Mesh& low, const Mesh& high, const BundleParams& params,
                                       ProgressSink* progress = nullptr,
                                       const CancelToken* cancel = nullptr);

}  // namespace cyber::exportbundle
