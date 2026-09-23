#pragma once

#include <cstddef>
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
    // Bake one file per OCCUPIED UDIM TILE of the low-poly's UV layout instead
    // of one file per map (surface-baking spec, "UDIM-aware baking"). Off by
    // default: a layout inside the unit square is tile 1001 and produces exactly
    // the same file either way, but a host has to ASK for a multi-file map set
    // before it gets one.
    //
    // A UDIM bundle whose layout occupies more than one tile REQUIRES the
    // preset's naming pattern to carry `{udim}`; without it every tile would be
    // written to one path, each overwriting the last, while the report listed
    // them all.
    bool udim = false;
    // The host's texel ceiling, in pixels, applied to every map this bundle
    // bakes -- PER TILE and, for a UDIM bundle, IN AGGREGATE over the occupied
    // tiles, with the same rule and the same words bake::bakeUdim() uses. 0
    // means no ceiling, which is what a caller with no host policy (the CLI)
    // passes.
    //
    // It lives here rather than being read from a global because the ceiling is
    // the EMBEDDER's policy: the C ABI sets it from cyber_max_bake_pixels(), and
    // a UDIM bundle multiplies the exposure by the occupied-tile count, so a
    // ceiling that stopped a single 8K map has to stop ten of them too.
    std::size_t maxPixels = 0;
    // The working-set bound every map is baked under (surface-baking,
    // "Regioned baking with a bounded working set"), in texels of output image
    // held in flight. 0 (the default) bakes each map whole and writes it in one
    // piece, exactly as before. Non-zero bakes each map in regions and STREAMS
    // it to its file band by band, through the preset's green flip and colour
    // encoding, so no map is ever held whole; the files are byte-identical
    // either way. The regioned bake's scratch file lives beside the mesh, on
    // the disk the bundle is written to. It never refuses a bundle.
    std::size_t maxWorkingSetTexels = 0;
};

// How one map file was produced, region by region. A map baked whole reports
// one region of its full height with no halo; the mesh entry reports zeros.
struct BundleRegions {
    std::size_t regionCount = 0;
    int regionRows = 0;
    int haloRows = 0;
    std::size_t workingSetTexels = 0;
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
    // The UDIM tile this map holds, under the `1001 + u + 10*v` numbering. 1001
    // for a map baked over the unit square and for the mesh entry, because the
    // unit square IS tile 1001 -- a report row therefore names a tile whether or
    // not the bundle was UDIM-aware.
    int udimTile = 1001;
    // How the map was produced, region by region.
    BundleRegions regions;
};

struct BundleResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::vector<BundleFile> files;
    std::vector<std::string> warnings;
    // The occupied UDIM tiles of the low-poly's layout, ascending by number, and
    // the faces whose UVs no tile number can address. Detected BEFORE any bake
    // runs and reported whether or not `BundleParams::udim` was set, so a host
    // can see a multi-tile layout it did not ask to bake as one -- and so a
    // refusal names what it was asked for. Empty only when the low-poly carries
    // no UV layout at all.
    std::vector<int> udimTiles;
    std::size_t udimUnaddressableFaces = 0;
    // Set when the low-poly carried no UVs and the bundle unwrapped it.
    bool unwrapped = false;
    int chartCount = 0;
    float maxAngleDistortion = 0.0f;
};

// Whether any map `preset` writes READS BundleParams::placement, and therefore
// whether an unusable placement refuses this bundle. Public so that the entry
// points which validate parameters BEFORE calling writeBundle -- the C ABI and
// the CLI -- apply the engine's own "checked only for a map that reads it" rule
// to a whole preset instead of guessing at the map set.
[[nodiscard]] bool presetReadsPlacement(const io::ExportPreset& preset);

// Writes the bundle. `low` is modified in place when it needs UVs: baking is
// impossible without them, and requiring the caller to pre-unwrap would make
// `--preset` useless on a freshly remeshed mesh. `cancel` covers that unwrap as
// well as the bakes, so the phase that dominates the wall clock on a UV-less
// low-poly is interruptible; cancelling during it leaves `low` untouched.
[[nodiscard]] BundleResult writeBundle(Mesh& low, const Mesh& high, const BundleParams& params,
                                       ProgressSink* progress = nullptr,
                                       const CancelToken* cancel = nullptr);

}  // namespace cyber::exportbundle
