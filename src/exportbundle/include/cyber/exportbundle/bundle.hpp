#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cyber/bake/bake.hpp"
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
    // Frame BakeMap::BentNormal is expressed in, and the factor
    // BakeMap::Thickness multiplies its mean depth by -- the same defaults and
    // the same meaning BakeParams gives them.
    //
    // The UP AXIS is deliberately NOT here: a preset already declares the axis
    // its target app expects (ExportPreset::upAxis), and that is what the
    // object-space maps are baked in. An unrecognised value is reported as a
    // warning and treated as y-up rather than guessed at.
    bake::NormalSpace bentNormalSpace = bake::NormalSpace::Tangent;
    float thicknessScale = 2.0f;
    // Border-padding radius in texels for every map the bundle bakes -- the
    // same default and the same meaning BakeParams gives it. A preset does NOT
    // declare one: padding repairs an artefact of the UV layout, not of the
    // target app's conventions, and every app wants it.
    int paddingRadius = 8;
    // The object->world placement BakeMap::WorldDirection carries its normals
    // through, and the normalization BakeMap::UvDensity uses -- the same
    // defaults and the same meaning BakeParams gives them. Neither belongs on a
    // PRESET, for the same reason the padding radius does not: a placement says
    // where the asset sits in a scene and a normalization says what question the
    // density map is answering, and neither is a target app's convention.
    bake::PlacementMatrix placement = bake::identityPlacement();
    bake::DensityNormalization densityNormalization = bake::DensityNormalization::Absolute;
};

struct BundleFile {
    std::string path;
    std::string kind;        // "mesh", or the preset map name ("normal", "ao", ...)
    std::string colorSpace;  // "linear" | "srgb" — the encoding actually written
    int width = 0;           // 0 for the mesh
    int height = 0;
    // What the pixels mean, as the bake reported it. The mesh entry keeps the
    // default (EncodingBasis::None): an encoded map is not interpretable
    // without this, so it travels with the file record rather than being
    // re-derived from the map's name.
    bake::BakeEncoding encoding;
    // What the border-padding stage did. The mesh entry keeps the default
    // (PaddingMode::None, radius 0).
    bake::BakePadding padding;
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
// `--preset` useless on a freshly remeshed mesh. `cancel` covers that unwrap as
// well as the bakes, so the phase that dominates the wall clock on a UV-less
// low-poly is interruptible; cancelling during it leaves `low` untouched.
[[nodiscard]] BundleResult writeBundle(Mesh& low, const Mesh& high, const BundleParams& params,
                                       ProgressSink* progress = nullptr,
                                       const CancelToken* cancel = nullptr);

}  // namespace cyber::exportbundle
