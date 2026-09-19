#include "cyber/exportbundle/bundle.hpp"

#include <optional>
#include <unordered_set>

#include "cyber/bake/bake.hpp"
#include "cyber/core/io.hpp"
#include "cyber/imageio/image.hpp"
#include "cyber/uv/atlas.hpp"

namespace cyber::exportbundle {

namespace {

using io::ColorSpace;
using io::ExportPreset;
using io::GreenChannel;
using io::PresetMap;
using io::PresetMapEntry;

std::optional<bake::BakeMap> toBakeMap(PresetMap map) {
    switch (map) {
        case PresetMap::Normal:
            return bake::BakeMap::Normal;
        case PresetMap::AmbientOcclusion:
            return bake::BakeMap::AmbientOcclusion;
        case PresetMap::Curvature:
            return bake::BakeMap::Curvature;
        case PresetMap::Cavity:
            return bake::BakeMap::Cavity;
        case PresetMap::Displacement:
            return bake::BakeMap::Displacement;
        case PresetMap::Color:
            return bake::BakeMap::Color;
        case PresetMap::Position:
            return bake::BakeMap::Position;
        case PresetMap::ObjectNormal:
            return bake::BakeMap::ObjectNormal;
        case PresetMap::ObjectPosition:
            return bake::BakeMap::ObjectPosition;
        case PresetMap::BentNormal:
            return bake::BakeMap::BentNormal;
        case PresetMap::Thickness:
            return bake::BakeMap::Thickness;
        case PresetMap::MaterialId:
            return bake::BakeMap::MaterialId;
        case PresetMap::ObjectId:
            return bake::BakeMap::ObjectId;
    }
    return std::nullopt;
}

// An id map's texels are exact keys. linearToSrgb() is a non-linear remap, so
// applying it rewrites every id's colour and the file stops matching the table
// the bake reported — the whole point of the map. Refused with a warning, the
// way the bundle already refuses sRGB into an EXR.
bool isIdMap(PresetMap map) { return map == PresetMap::MaterialId || map == PresetMap::ObjectId; }

// The axis the object-space maps are baked in comes from the preset, which is
// where "what this target app expects" already lives. An unrecognised value is
// reported rather than guessed at: a silently wrong axis produces a map that
// looks plausible and shades inside out.
bool usesObjectSpace(const ExportPreset& preset) {
    for (const PresetMapEntry& entry : preset.maps) {
        if (entry.map == PresetMap::ObjectNormal || entry.map == PresetMap::ObjectPosition ||
            entry.map == PresetMap::BentNormal) {
            return true;
        }
    }
    return false;
}

bake::UpAxis presetUpAxis(const ExportPreset& preset, BundleResult& result) {
    if (preset.upAxis == "z-up") {
        return bake::UpAxis::ZUp;
    }
    if (preset.upAxis != "y-up" && usesObjectSpace(preset)) {
        result.warnings.push_back("preset '" + preset.name + "' declares up axis '" +
                                  preset.upAxis +
                                  "', which is neither 'y-up' nor 'z-up'; object-space maps are "
                                  "baked y-up");
    }
    return bake::UpAxis::YUp;
}

// One map's slice of the bundle's progress. The ray-traced maps report per
// texel, so a single 4K bent-normal bake is the difference between a moving bar
// and a host that looks hung for a minute. A sink with no callback behind it
// (no host progress at all) reports nothing and costs nothing, so the caller
// always has a sink to hand the bake.
ProgressSink mapSubrange(ProgressSink* progress, float done, float total) {
    if (progress == nullptr) {
        return ProgressSink{};
    }
    return progress->subrange(done / total, (done + 1.0f) / total, "export bundle");
}

// DirectX-style normal maps point green down. The bake always produces the
// OpenGL convention, so the flip happens once, here, on the encoded texel.
void flipGreen(bake::Image& image) {
    if (image.channels < 2) {
        return;
    }
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            image.at(x, y, 1) = 1.0f - image.at(x, y, 1);
        }
    }
}

void encodeSrgb(bake::Image& image) {
    // Alpha, where present, stays linear by convention.
    const int colorChannels = image.channels == 4 ? 3 : image.channels;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            for (int c = 0; c < colorChannels; ++c) {
                image.at(x, y, c) = io::linearToSrgb(image.at(x, y, c));
            }
        }
    }
}

bool ensureUvs(Mesh& low, BundleResult& result, const CancelToken* cancel) {
    if (low.cornerAttributes().find<Vec2>(io::kUvAttribute) != nullptr) {
        return true;
    }
    // The unwrap is routinely the longest phase of the whole bundle (its chart
    // merge trial-unwraps candidate chart unions), so the caller's token has to
    // reach it — otherwise the cancel this module advertises cannot touch the
    // one phase a user would most want to abort. It is left out of the progress
    // channel on purpose: the map loop below owns [0,1] of that sink.
    const uv::AtlasResult atlas = uv::unwrapAtlas(low, {}, nullptr, cancel);
    if (atlas.cancelled) {
        result.cancelled = true;
        return false;
    }
    if (!atlas.ok) {
        result.error = "the low-poly carries no UVs and automatic unwrapping failed";
        return false;
    }
    result.unwrapped = true;
    result.chartCount = atlas.chartCount;
    result.maxAngleDistortion = atlas.maxAngleDistortion;
    return true;
}

// Whether a map path stays under the caller's output directory. The file name
// comes from the preset's namingPattern, which is DATA from a possibly
// third-party file, so containment is checked before anything is written. The
// test is purely lexical: no filesystem access, so it holds for a directory
// that does not exist yet and does not depend on symlink resolution.
bool isInsideDirectory(const std::filesystem::path& directory, const std::filesystem::path& path) {
    const std::filesystem::path base =
        (directory.empty() ? std::filesystem::path(".") : directory).lexically_normal();
    const std::filesystem::path relative = path.lexically_normal().lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

// Applies the preset's conventions to a freshly baked map and writes it.
bool writeMap(const ExportPreset& preset, const PresetMapEntry& entry, bake::Image image,
              const bake::BakeEncoding& encoding, const std::filesystem::path& path,
              BundleResult& result) {
    if (entry.map == PresetMap::Normal && preset.normalGreen == GreenChannel::MinusY) {
        flipGreen(image);
    }
    // sRGB is an encoding for 8-bit containers. EXR stores float linear by
    // convention, so a preset asking for sRGB there is reported, never applied
    // silently in either direction.
    std::string writtenSpace = "linear";
    if (entry.colorSpace == ColorSpace::Srgb) {
        if (isIdMap(entry.map)) {
            result.warnings.push_back(std::string("map '") + io::presetMapName(entry.map) +
                                      "' is an id map, whose texels are exact keys; the "
                                      "declared sRGB encoding would change every id's colour "
                                      "and is not applied");
        } else if (preset.textureFormat == "exr") {
            result.warnings.push_back(std::string("map '") + io::presetMapName(entry.map) +
                                      "' declares sRGB but the preset writes EXR; "
                                      "written as linear float");
        } else {
            encodeSrgb(image);
            writtenSpace = "srgb";
        }
    }

    const std::string ext =
        path.has_extension() ? path.extension().string().substr(1) : std::string();
    if (!ext.empty() && ext != preset.textureFormat) {
        result.warnings.push_back(std::string("map '") + io::presetMapName(entry.map) +
                                  "' is named '." + ext + "' but the preset writes " +
                                  preset.textureFormat + "; writing " + preset.textureFormat);
    }

    const int width = image.width;
    const int height = image.height;
    // The preset, not the file name, picks the container: the extension-
    // dispatching saveImage() overload would otherwise switch the encoder
    // behind the color-space policy chosen just above, writing sRGB bytes into
    // a float EXR or clamping a float map into 8-bit PNG.
    const imageio::ImageFormat format =
        preset.textureFormat == "exr" ? imageio::ImageFormat::Exr : imageio::ImageFormat::Png;
    if (!imageio::saveImage(path.string(), image, format)) {
        result.error = "cannot write map '" + path.string() + "'";
        return false;
    }
    result.files.push_back(BundleFile{path.string(), io::presetMapName(entry.map), writtenSpace,
                                      width, height, encoding});
    return true;
}

}  // namespace

BundleResult writeBundle(Mesh& low, const Mesh& high, const BundleParams& params,
                         ProgressSink* progress, const CancelToken* cancel) {
    BundleResult result;
    const ExportPreset& preset = params.preset;

    const std::string meshExt = params.meshPath.has_extension()
                                    ? params.meshPath.extension().string().substr(1)
                                    : std::string();
    if (!meshExt.empty() && meshExt != preset.meshFormat) {
        result.warnings.push_back("preset '" + preset.name + "' specifies mesh format '" +
                                  preset.meshFormat + "' but the output path is '." + meshExt +
                                  "'; writing '." + meshExt + "' as given");
    }

    if (!ensureUvs(low, result, cancel)) {
        return result;
    }

    const io::Status exported = io::exportMesh(low, params.meshPath);
    if (!exported.ok()) {
        result.error = exported.error().message;
        return result;
    }
    result.files.push_back(BundleFile{params.meshPath.string(), "mesh", "", 0, 0, {}});

    const std::string basename =
        params.basename.empty() ? params.meshPath.stem().string() : params.basename;
    const std::filesystem::path directory = params.meshPath.parent_path();

    bake::BakeParams bakeParams;
    bakeParams.width = preset.resolution;
    bakeParams.height = preset.resolution;
    bakeParams.cageDistance = params.cageDistance;
    bakeParams.aoSamples = params.aoSamples;
    bakeParams.aoRadius = params.aoRadius;
    bakeParams.bentNormalSpace = params.bentNormalSpace;
    bakeParams.thicknessScale = params.thicknessScale;
    bakeParams.upAxis = presetUpAxis(preset, result);

    const auto total = static_cast<float>(preset.maps.size());
    float done = 0.0f;
    // parsePreset refuses a preset file whose maps share a name; a preset built
    // in code reaches here unchecked, and the damage is silent -- one file
    // holding the last bake, two report rows claiming it under different kinds.
    std::unordered_set<std::string> written;
    for (const PresetMapEntry& entry : preset.maps) {
        if (cancel != nullptr && cancel->isCancelled()) {
            result.cancelled = true;
            return result;
        }
        const std::optional<bake::BakeMap> map = toBakeMap(entry.map);
        if (!map.has_value()) {
            result.error = std::string("preset map '") + io::presetMapName(entry.map) +
                           "' has no bake implementation";
            return result;
        }
        ProgressSink mapProgress = mapSubrange(progress, done, total);
        bake::BakeResult baked = bake::bake(low, high, *map, bakeParams, &mapProgress, cancel);
        if (baked.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (baked.image.pixels.empty()) {
            result.error = std::string("bake produced no image for map '") +
                           io::presetMapName(entry.map) + "'";
            return result;
        }
        // An empty expansion is presetMapFileName refusing a name that would
        // leave the output directory; joining it would write the directory
        // itself, so it is an error here and not a path.
        const std::string fileName = io::presetMapFileName(preset, entry, basename);
        if (fileName.empty()) {
            result.error = std::string("map '") + io::presetMapName(entry.map) +
                           "' names a file outside the output directory; check the preset's "
                           "namingPattern, name and suffixes, and the basename";
            return result;
        }
        const std::filesystem::path path = directory / fileName;
        if (!isInsideDirectory(directory, path)) {
            result.error = "map '" + path.string() +
                           "' resolves outside the output directory; check the preset's "
                           "namingPattern and the basename";
            return result;
        }
        if (!written.insert(path.string()).second) {
            result.error = std::string("map '") + io::presetMapName(entry.map) +
                           "' would overwrite '" + path.string() +
                           "', already written by an earlier map; the preset's namingPattern "
                           "and suffixes must give every map its own name";
            return result;
        }
        if (!writeMap(preset, entry, std::move(baked.image), baked.encoding, path, result)) {
            return result;
        }
        done += 1.0f;
        if (progress != nullptr) {
            progress->report(done / total, "export bundle");
        }
    }

    result.ok = true;
    return result;
}

}  // namespace cyber::exportbundle
