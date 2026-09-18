#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cyber/bake/field_evaluator.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/imageio/load.hpp"

// High-to-low surface baking (surface-baking spec). Bakes detail from a Target
// (high-poly) onto the UV layout of an EditMesh (low-poly). Ray casting is
// dispatched through the compute-acceleration layer, so the CPU backend is the
// reference and a GPU backend accelerates it transparently (spec: "Accelerated,
// cancellable baking"). Cooperative cancellation leaves the caller free to keep
// the previous maps.
namespace cyber::bake {

enum class BakeMap {
    Normal,            // tangent-space normal map (RGB, encoded [0,1])
    AmbientOcclusion,  // openness in [0,1] (1 = fully lit), single channel
    Displacement,      // signed height along the low-poly normal, single channel
    Position,          // high-poly hit position in MODEL UNITS, unencoded (RGB)
    Color,             // Target vertex color sampled at the hit (RGB)
    Curvature,         // signed mean curvature around mid-gray, single channel
    Cavity,            // concavity only (white = flat or convex), single channel
    // --- appended in 0.8.0; the values above keep their numbers -----------
    ObjectNormal,    // Target normal in object space, encoded n*0.5+0.5 (RGB)
    ObjectPosition,  // the Position hit point rescaled over the bake bounds (RGB)
    BentNormal,      // mean unoccluded hemisphere direction, encoded n*0.5+0.5 (RGB)
    Thickness,       // material behind the surface, in model units, single channel
};

// Axis convention the OBJECT-SPACE maps are expressed in. YUp is this engine's
// own convention (and glTF's); ZUp re-expresses a vector as (x, -z, y), which is
// what a z-up DCC reads. It applies to ObjectNormal, ObjectPosition and, when
// BentNormal is baked in object space, to that too. Tangent space has no up
// axis, so it is ignored there.
enum class UpAxis {
    YUp,
    ZUp,
};

// Frame a bent-normal bake is expressed in. Tangent (the default) matches
// BakeMap::Normal, so the two maps drop into the same shader slot; Object gives
// a direction a smart mask can compare against a world direction.
enum class NormalSpace {
    Tangent,
    Object,
};

// What the numbers in a baked image MEAN. An encoded map without its basis is a
// picture of some numbers: an object-space position cannot be turned back into a
// coordinate without the box it was rescaled over, and a thickness cannot be
// turned back into a distance without the factor it was multiplied by.
enum class EncodingBasis {
    None,           // raw values (AO, color, curvature, cavity)
    TangentNormal,  // unit direction in the texel's tangent frame, v*0.5+0.5
    ObjectNormal,   // unit direction in object space (`upAxis`), v*0.5+0.5
    ObjectBounds,   // object-space position rescaled over [boundsMin, boundsMax]
    Distance,       // a length in model units, multiplied by `scale`
};

// Filled by every bake, for every map. Members that do not apply to the basis
// keep their defaults (a None basis says nothing beyond "these are the values").
struct BakeEncoding {
    EncodingBasis basis = EncodingBasis::None;
    UpAxis upAxis = UpAxis::YUp;
    // The box an ObjectBounds map was rescaled over, already expressed in
    // `upAxis`, so a consumer decodes with (min + value * (max - min)) without
    // re-deriving the swizzle.
    Vec3 boundsMin;
    Vec3 boundsMax;
    // The factor a Distance map was multiplied by (BakeParams::thicknessScale
    // for Thickness; 1 for Displacement).
    float scale = 1.0f;
};

struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> pixels;  // row-major, size = width*height*channels

    [[nodiscard]] float& at(int x, int y, int c) {
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)) *
                          static_cast<std::size_t>(channels) +
                      static_cast<std::size_t>(c)];
    }
    [[nodiscard]] float at(int x, int y, int c) const {
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)) *
                          static_cast<std::size_t>(channels) +
                      static_cast<std::size_t>(c)];
    }
};

// Selects where BakeMap::Color reads the Target's color from. VertexColors
// (the default) samples the Target's per-vertex "color" attribute; Texture
// samples `texture` at the Target UV interpolated from the hit face's per-corner
// "uv" attribute. Texture falls back to vertex colors when `texture` is null or
// the Target carries no UVs, so existing bakes are unchanged (roadmap 11.1).
struct ColorSource {
    enum Kind { VertexColors, Texture };
    Kind kind = VertexColors;
    const cyber::imageio::LoadedImage* texture = nullptr;
};

// Numeric members are range-checked by bake() against the ranges named below,
// but only for the maps that read them: a value out of range is a degenerate
// request and yields an empty image rather than a map of NaN.
struct BakeParams {
    int width = 512;   // > 0
    int height = 512;  // > 0
    // Projection cage: rays start at (surface + normal*cageDistance) and cast
    // inward up to 2*cageDistance to find the Target (11.2 makes this a
    // per-vertex editable cage; here it is a uniform distance). Finite, >= 0.
    float cageDistance = 0.1f;
    // Hemisphere rays per texel for AO. Binary visibility quantizes openness to
    // aoSamples+1 rungs, so a low budget ships a visibly stepped map even with
    // the per-texel sample rotation dithering it; 64 is the smallest default
    // that reads as continuous in 8-bit. > 0 on the raycast path, which divides
    // the occluded-ray count by it.
    int aoSamples = 64;
    float aoRadius = 1.0f;    // an AO ray hit beyond this does not occlude; finite, >= 0
    float aoBias = 1e-3f;     // start offset to avoid self-hits; finite
    ColorSource colorSource;  // BakeMap::Color source (default: Target vertex colors)
    // Curvature magnitude (units 1/length) that saturates a Curvature/Cavity
    // bake to full white/black. 0 = auto: the 95th percentile of |curvature|
    // over the Target, which is scale-independent and keeps one pinched vertex
    // from flattening the map to mid-gray. Finite (a negative value is auto too).
    float curvatureRange = 0.0f;
    // Exact ceiling on output texels. Zero disables it. This is intentionally
    // separate from dimensions: an image can be tall, wide or square while a
    // host's allocation budget is about their product.
    std::size_t maxPixels = 0;
    // Axis convention for the object-space maps (ObjectNormal, ObjectPosition,
    // and BentNormal when bentNormalSpace is Object). Default y-up: the
    // engine's own convention. Recorded in BakeResult::encoding.
    UpAxis upAxis = UpAxis::YUp;
    // Frame BakeMap::BentNormal is expressed in. Default tangent, matching
    // BakeMap::Normal. Recorded in BakeResult::encoding.
    NormalSpace bentNormalSpace = NormalSpace::Tangent;
    // Factor BakeMap::Thickness multiplies its mean back-facing depth by.
    // ArmorPaint doubles the distance and the default matches it, but the
    // doubling is a CHOICE (the mean chord of a cosine-weighted hemisphere
    // through a slab of thickness d is 2d, so it undoes the hemisphere's own
    // averaging for that case) and an inherited constant nobody can see is how
    // a bake becomes unreproducible. Finite, >= 0; read only by Thickness.
    float thicknessScale = 2.0f;
    // Optional field evaluator (pipeline-bridge spec, "Field-sampled baking").
    // When set, Normal / AmbientOcclusion / Curvature / Cavity sample the field
    // directly — the cage ray is sphere-traced through it and normals come from
    // exact gradients instead of interpolated mesh normals. Every other map,
    // and every bake with `field == nullptr`, takes the raycast path with
    // BIT-IDENTICAL output. `highPoly` may be empty only when an evaluator is
    // attached and the requested map is one of the four it covers.
    const FieldEvaluator* field = nullptr;
};

struct BakeResult {
    Image image;
    // What the pixels mean. Filled for every map, so a consumer never has to
    // infer an encoding from the map's name.
    BakeEncoding encoding;
    bool cancelled = false;
    std::size_t texelsCovered = 0;  // texels touched by the UV layout
    // Set when a FIELD EVALUATOR broke its contract -- a NaN distance, a
    // non-finite gradient or curvature, an openness outside [0,1]. The bake is
    // abandoned and `image` is empty: a host's broken callback must not come
    // back as a plausible map. Empty message when nothing was violated.
    bool fieldContractViolated = false;
    std::string fieldContractMessage;
    // Samples a CORRECT field left undefined -- an infinite distance (the
    // ordinary "nothing here" sentinel) or a zero-length gradient (no gradient
    // exists on an SDF's medial axis). Not a failure; counted so a host can see
    // how much of its field the bake could not reach.
    std::size_t fieldUndefinedSamples = 0;
};

// Bakes `map` from `highPoly` onto the per-corner "uv" layout of `lowPoly`.
// `lowPoly` must carry a Vec2 "uv" corner attribute; missing UVs or a
// degenerate request — including a BakeParams member outside the range
// documented above — yield an empty (zero-size) image.
[[nodiscard]] BakeResult bake(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map,
                              const BakeParams& params, ProgressSink* progress = nullptr,
                              const CancelToken* cancel = nullptr);

}  // namespace cyber::bake
