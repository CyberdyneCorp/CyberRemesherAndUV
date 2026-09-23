#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    // --- appended in 0.8.0; the values above keep their numbers -----------
    MaterialId,  // one flat colour per Target `material_id` (RGB, exact)
    ObjectId,    // one flat colour per Target object/submesh (RGB, exact)
    // --- appended in 0.9.0; the values above keep their numbers -----------
    // The Target normal carried into WORLD space by BakeParams::placement,
    // encoded n*0.5+0.5 (RGB). This engine has one model space, so with an
    // IDENTITY placement this map is bit-identical to ObjectNormal on purpose:
    // the two differ by the placement transform and by nothing else.
    WorldDirection,
    // Texels per unit of SURFACE AREA that the EditMesh's UV layout gives the
    // surface under each texel, at the requested resolution (1 channel). A
    // property of the UV layout and the resolution alone -- it does not read
    // the Target. Zero is the documented sentinel for "no density here".
    UvDensity,
};

// How a UvDensity bake normalizes its values. Absolute is what a scale-locked
// material needs (a real-world texel scale it can hold across islands);
// Relative is what shows an artist that one island is packed differently from
// the rest. Recorded in BakeResult::encoding either way, together with the mean
// that was measured, so a relative map converts back to an absolute one.
enum class DensityNormalization {
    Absolute,
    Relative,
};

// The affine object->world placement a host has applied to put the asset in its
// scene: a 4x4 ROW-MAJOR matrix, m[row * 4 + column], identity by default. A
// 4x4 because that is the shape every DCC and scene graph already hands out, so
// nothing has to be decomposed on the way in.
//
// Only the upper-left 3x3 is read today, because the only map that reads a
// placement is a DIRECTION map and a direction is unaffected by translation.
// The translation is accepted and recorded so that a world-space POSITION map,
// if one is ever added, needs no second parameter of a different shape.
using PlacementMatrix = std::array<float, 16>;

[[nodiscard]] constexpr PlacementMatrix identityPlacement() {
    return PlacementMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// Whether `placement` is a placement at all: every element finite, and the
// upper-left 3x3 invertible. A singular linear part collapses every direction
// onto a plane or a point and has no inverse transpose to carry a normal by.
//
// Public because the spec requires every entry point that validates parameters
// to validate this one IDENTICALLY, and three copies of a determinant are three
// chances to disagree about what "singular" means.
[[nodiscard]] bool placementUsable(const PlacementMatrix& placement);

// Whether `map` READS BakeParams::placement. Public, and the only place the set
// is written down, because the spec checks a placement only for a map that
// reads one -- a bake that reads no placement is not refused because of what an
// untouched field happens to hold -- and that rule has to be the SAME rule at
// the C ABI, the export bundle and the CLI. An entry point that kept its own
// copy of the set would drift the day a second map starts reading a placement.
[[nodiscard]] bool mapReadsPlacement(BakeMap map);

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
    // An EXACT key, not a measurement: every covered texel holds one of the
    // colours in BakeEncoding::idColors verbatim. Never filter, resample or
    // colour-convert such a map -- compare it at zero tolerance.
    IdColor,
    // --- appended in 0.9.0; the values above keep their numbers, because the
    // C ABI's CyberEncodingBasis mirrors this enum one for one -------------
    // Unit direction in WORLD space: the object-space direction carried through
    // BakeEncoding::placement, then expressed in `upAxis`, then v*0.5+0.5.
    // Distinct from ObjectNormal because the placement is what separates the
    // two spaces in an engine that otherwise has only one.
    WorldDirection,
    // Texels per SQUARE model unit, in BakeEncoding::densityNormalization. The
    // linear "texels per unit of length" convention is its square root. Zero
    // means the density is UNDEFINED there (a face with no UV area or no
    // surface area, or a texel the bake wrote nothing to), never "zero
    // density": a defined density is a positive UV area over a positive surface
    // area and is strictly positive.
    UvDensity,
};

// One row of the id-to-colour table an id map reports. The colour is the exact
// 8-bit triple written for `id`; the float in the image is `channel / 255`.
// Reported for every distinct id on the Target, whether or not the UV layout
// happens to show it, because a consumer resolving a picked colour needs the
// whole key.
struct IdColorEntry {
    std::int32_t id = 0;
    std::array<std::uint8_t, 3> color{};
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
    // The range this map's OWN encoding guarantees, on every channel of every
    // texel it writes: an object-space position is (p - min)/(max - min) and
    // never leaves [0,1]; an occlusion is a fraction of a hemisphere; a
    // thickness is a distance and is never negative. A map whose encoding
    // guarantees nothing -- a position in model units, a signed displacement,
    // a colour taken verbatim from the Target -- leaves these infinite.
    //
    // Border padding is confined to this range (see the surface-baking spec,
    // "Bake output padding across UV island borders"): a padded texel outside it
    // is not a continuation of the map, it is a value the map's own contract
    // says cannot occur, and every consumer that decodes the map -- `min + v *
    // (max - min)` for an object-space position -- is entitled to assume it
    // cannot.
    float valueMin = -std::numeric_limits<float>::infinity();
    float valueMax = std::numeric_limits<float>::infinity();
    // Which of the Target's id columns an IdColor map actually read:
    // "material_id", "object_id", "group_id", "component" for the
    // face-connected-component fallback, or "none" when nothing declared an id
    // and every face reads 0. Empty for every other basis.
    std::string idSource;
    // The id-to-colour table, ASCENDING BY ID -- a stable ordered key, never a
    // container's iteration order. Empty for every other basis.
    std::vector<IdColorEntry> idColors;
    // The placement a WorldDirection map's normals were carried through (by its
    // inverse transpose), so a consumer can recover the object-space direction.
    // Identity for every map that does not read one, which is every other map.
    PlacementMatrix placement = identityPlacement();
    // How a UvDensity map was normalized, and the MEAN absolute density its
    // defined texels held. The mean is reported in BOTH modes: it converts a
    // relative map back to an absolute one, and it tells a host what an
    // absolute map's own average is. Zero when the map defined no texel.
    // Meaningless, and left at these defaults, for every other basis.
    DensityNormalization densityNormalization = DensityNormalization::Absolute;
    float densityMean = 0.0f;
};

// How a map's PADDED BAND -- the texels just outside each UV island -- was
// filled. The rule is decided by the map's EncodingBasis rather than by its
// name, so a map type added later gets the right one as long as it records an
// honest basis.
enum class PaddingMode {
    None,         // radius 0, or no covered texel to pad from
    Nearest,      // the nearest covered texel's value, copied VERBATIM
    Extrapolate,  // the gradient running off the island, continued outward
    // Continued, then renormalized to unit length: a direction map's padded
    // band has to decode to directions, not to shortened vectors.
    ExtrapolateUnit,
};

// What the padding stage did, reported with every map. A consumer reading an
// id map wants to see here that its band was copied and not interpolated.
struct BakePadding {
    int radius = 0;  // the radius applied, in texels; 0 = padding disabled
    PaddingMode mode = PaddingMode::None;
    std::size_t texelsFilled = 0;  // texels the band wrote
};

// The colour an id map writes for `id`, as the exact 8-bit triple that reaches
// the file. A pure function of the id computed with INTEGER arithmetic only, so
// the same id yields the same colour across runs, machines, compilers and
// standard libraries; see the change's design.md for why a float hash
// (ArmorPaint's frac(sin(...))) and an ordinal palette were both rejected.
// Every channel lands in [64, 255], which keeps (0,0,0) reserved for "no id"
// and unreachable from here.
[[nodiscard]] std::array<std::uint8_t, 3> idColor(std::int32_t id);

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
    // Exact ceiling on OUTPUT texels: the width * height of the map this request
    // produces (per tile, and in aggregate for a UDIM set). Zero disables it.
    // Intentionally separate from dimensions: an image can be tall, wide or
    // square while a host's budget is about their product.
    //
    // It bounds the OUTPUT, not the memory a bake holds in flight -- that is
    // maxWorkingSetTexels, below, and the two are deliberately separate: a host
    // that asked for a 16K map and has the disk for it is not refused because
    // the working set would not fit.
    std::size_t maxPixels = 0;
    // Bound on the texels of output image a REGIONED bake (bakeRegions) holds in
    // flight at once: one region plus its halo (see regionHaloRows). Zero means
    // no bound -- the whole output is one region, bit-identical to bake(). It
    // NEVER refuses: a bound below one row plus two halos yields one-row regions
    // and RegionPlan reports the working set actually held. The whole-image
    // entry points (bake, bakeUdim) hold the whole output by construction and do
    // not read it. The Target-side structures (BVH, Target normals, curvature
    // field) scale with the mesh, not the output, and are not counted.
    std::size_t maxWorkingSetTexels = 0;
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
    // Width of the padded band grown outward from every UV island, in TEXELS,
    // applied to every map before bake() returns. What the band has to cover is
    // measured in texels -- a bilinear tap needs 1, mip level k reaches 2^k, a
    // BC block is 4 -- so the radius is too, and a host that wants it to scale
    // with resolution scales it itself. The default of 8 covers the first three
    // mip levels and two compression blocks. 0 disables padding and returns the
    // map exactly as it was baked; NEGATIVE is refused, like every other
    // out-of-range parameter here.
    int paddingRadius = 8;
    // The object->world placement BakeMap::WorldDirection carries its normals
    // through, by its INVERSE TRANSPOSE -- a plain multiply is correct only for
    // a rotation and shears a normal off the surface under non-uniform scale.
    // Identity by default, which makes WorldDirection bit-identical to
    // ObjectNormal: this engine has one model space, and the placement is what
    // separates them.
    //
    // Every element must be finite and the upper-left 3x3 must be INVERTIBLE (a
    // singular linear part carries no direction anywhere); a placement that is
    // not is refused -- the bake returns no image -- rather than substituted
    // with the identity. Read ONLY by WorldDirection, so a bake of any other
    // map is unaffected by whatever is here.
    PlacementMatrix placement = identityPlacement();
    // How BakeMap::UvDensity normalizes its values. Read only by that map.
    DensityNormalization densityNormalization = DensityNormalization::Absolute;
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
    // What the border-padding stage did. Filled for every map.
    BakePadding padding;
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

// ---- UDIM (surface-baking spec, "UDIM-aware baking") ---------------------
//
// One tile of the standard UDIM grid: the tile whose UV origin is (u, v), so it
// covers [u, u+1) x [v, v+1). `number` is 1001 + u + 10*v, which makes the unit
// square tile 1001 -- and therefore makes an ordinary bake the tile-1001 case of
// a UDIM one rather than a different kind of thing.
struct UdimTile {
    int u = 0;
    int v = 0;
    int number = 1001;
};

// The grid that numbering can address. `u` is base-10 in the formula, so only
// [0, 9] has a tile number, and a negative `v` has none either. The upper bound
// on `v` is a CHOICE -- the numbering itself is unbounded -- and 999 reaches
// tile 10990, past every convention in use, while stopping a layout that holds
// v = 1000000 from being read as a request for a million images.
inline constexpr int kUdimMaxU = 9;
inline constexpr int kUdimMaxV = 999;

[[nodiscard]] constexpr int udimTileNumber(int u, int v) { return 1001 + u + 10 * v; }

// What a UV layout occupies, answerable WITHOUT baking: a host has to be able to
// show what it is about to allocate, and a refusal has to be able to name what
// it was asked for.
struct UdimLayout {
    // Occupied tiles, ASCENDING BY NUMBER -- a stable ordered key, never a
    // container's iteration order. A tile counts as occupied when a triangle of
    // the layout OVERLAPS it, not when a triangle's UV bounding box touches it:
    // every tile reported here is an image allocated, so an over-reporting test
    // would break the cost guarantee ("three tiles of a possible hundred cost
    // three") rather than merely waste a little work.
    std::vector<UdimTile> tiles;
    // Faces carrying a UV corner no 1001 + u + 10*v tile can address. Counted
    // rather than dropped: a layout authored in a convention this numbering
    // cannot express would otherwise bake with a quietly missing region.
    std::size_t unaddressableFaces = 0;
};

[[nodiscard]] UdimLayout udimTiles(const Mesh& mesh);

// Why a UDIM bake produced no images. `Parameters` is the same rejection bake()
// makes silently (missing UVs, a parameter out of range, no Target); the two
// ceilings are deliberately DISTINCT, because "this tile is too big" and "this
// many tiles of this size are too many" are different problems with different
// fixes and one message covering both tells a host neither.
enum class UdimRefusal {
    None,
    Parameters,
    NoOccupiedTiles,
    PerTileCeiling,
    AggregateCeiling,
    // A field evaluator broke its contract while a tile was being shaded. The
    // whole set is abandoned, as a broken contract abandons a single bake:
    // a host's broken callback must not come back as a plausible map, and it
    // must not come back as a plausible map for SOME of the tiles either.
    FieldContract,
};

// Which texel ceiling, if either, baking `tiles` tiles at these parameters
// trips, and the sentence that says so. `tiles` is 1 for an ordinary,
// single-image bake, which is why this answers for both.
//
// Public because the ceiling is not only bakeUdim's business: a caller that
// bakes a SET of maps -- the export bundle -- has to refuse BEFORE it writes
// the first file, and it must refuse with the same rule and the same words
// rather than a second, drifting copy of them. `maxPixels == 0` (no ceiling) or
// a degenerate size answers None.
struct UdimCeiling {
    UdimRefusal refusal = UdimRefusal::None;
    std::string message;  // empty when `refusal` is None
};

[[nodiscard]] UdimCeiling udimCeiling(const BakeParams& params, std::size_t tiles);

struct UdimTileBake {
    UdimTile tile;
    BakeResult result;
};

struct UdimBakeResult {
    // Filled even when the bake is refused: detection runs before baking starts.
    UdimLayout layout;
    // One entry per occupied tile, in the layout's order. Empty on a refusal or
    // a cancellation -- a partial set is never returned as though it succeeded.
    std::vector<UdimTileBake> tiles;
    UdimRefusal refusal = UdimRefusal::None;
    std::string refusalMessage;
    bool cancelled = false;
};

// Bakes `map` once per occupied tile of `lowPoly`'s UV layout. The acceleration
// structure over `highPoly` is built ONCE and shared by every tile, so the rays
// cast for ambient occlusion, bent normal and thickness see the WHOLE mesh
// whatever tile is being written -- geometry whose UVs lie in another tile still
// occludes. Progress covers the whole set and cancellation is polled between
// tiles as well as inside one.
[[nodiscard]] UdimBakeResult bakeUdim(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map,
                                      const BakeParams& params, ProgressSink* progress = nullptr,
                                      const CancelToken* cancel = nullptr);

// ---- regioned baking (surface-baking spec, "Regioned baking with a bounded
// working set") ---------------------------------------------------------------
//
// A bake produced in full-width horizontal REGIONS whose finished rows are
// handed to a RegionSink in ascending order, each row exactly once, so the whole
// output never exists in memory. The assembled rows equal bake()/bakeUdim()'s
// image texel for texel, padded band and global normalizations included.

// How far a screen-space derivative reaches: the neighbouring texel. No map in
// this engine takes one today (curvature and cavity read the Target's curvature
// field at the cage hit), but the overlap is declared and honoured so that a
// derivative bake added later inherits a correct one instead of a region-local
// derivative that shows as a line on every seam.
inline constexpr int kDerivativeFootprintTexels = 1;

// The rows each assembly window overlaps its region by, above and below:
// max(2 * paddingRadius, kDerivativeFootprintTexels). TWICE the padding radius,
// because continuing a gradient reads the covered neighbour and the texel beyond
// it, so a band texel k rings out depends on texels up to 2k away.
[[nodiscard]] int regionHaloRows(const BakeParams& params);

// How a request divides into regions, answerable without baking.
struct RegionPlan {
    int regionRows = 0;  // rows per region (the last may be shorter)
    int haloRows = 0;    // overlap above and below each assembly window
    std::size_t regionCount = 0;
    // Peak texels of output image in flight: the tallest assembly window times
    // the width. Equal to width * height for a one-region bake.
    std::size_t workingSetTexels = 0;
    // False when maxWorkingSetTexels was below one row plus two halos, so the
    // working set held exceeds the bound. Reported, never refused.
    bool boundReached = true;
};

// The plan for ONE image of `params`' size. A degenerate size plans nothing.
[[nodiscard]] RegionPlan planRegions(const BakeParams& params);

// One band of finished output rows.
struct RegionRows {
    UdimTile tile;   // 1001 for an ordinary bake
    int width = 0;   // the full output width
    int height = 0;  // the full output height
    int channels = 0;
    int rowBegin = 0;               // first output row held
    int rowCount = 0;               // rows held
    const float* pixels = nullptr;  // rowCount * width * channels floats, row-major
    // The map's final encoding record, as the whole-image bake reports it.
    const BakeEncoding* encoding = nullptr;
};

// Where a regioned bake's rows go. An abstract class rather than a callback so
// an implementation can carry state (an open file, a row counter) and so the C
// ABI can wrap its own callback in one.
class RegionSink {
public:
    RegionSink() = default;
    RegionSink(const RegionSink&) = delete;
    RegionSink& operator=(const RegionSink&) = delete;
    RegionSink(RegionSink&&) = delete;
    RegionSink& operator=(RegionSink&&) = delete;
    virtual ~RegionSink() = default;

    // Receives rows in ascending order, each output row exactly once, tiles in
    // layout order. Returning false abandons the bake (an I/O failure, say).
    [[nodiscard]] virtual bool consume(const RegionRows& rows) = 0;
};

// What one output image of a regioned bake reported: everything a BakeResult
// carries except the pixels, which went to the sink.
struct RegionedTile {
    UdimTile tile;
    int width = 0;
    int height = 0;
    int channels = 0;
    BakeEncoding encoding;
    BakePadding padding;
    std::size_t texelsCovered = 0;
    std::size_t fieldUndefinedSamples = 0;
};

// Why a regioned bake produced no output beyond a refusal or a cancellation.
enum class RegionFailure {
    None,
    Sink,     // the sink returned false
    Scratch,  // the scratch storage could not be created, written or read
};

struct RegionedBakeResult {
    UdimLayout layout;  // filled even on a refusal, as bakeUdim's is
    // One entry per baked image, in layout order. Empty on a refusal, a
    // cancellation or a failure.
    std::vector<RegionedTile> tiles;
    UdimRefusal refusal = UdimRefusal::None;
    std::string refusalMessage;
    bool cancelled = false;
    RegionFailure failure = RegionFailure::None;
    std::string failureMessage;
    RegionPlan plan;
    // Measured, not planned: the largest number of output-image texels held in
    // one buffer at any point of the bake.
    std::size_t peakTexelsInFlight = 0;
};

// Bakes `map` in regions. `udim` false bakes the unit square (tile 1001) as
// bake() does; true bakes every occupied tile as bakeUdim() does, sharing one
// Target context and one density mean across the set. Every parameter bake()
// validates is validated identically, and the texel ceiling bounds the output.
//
// A set of more than one region shades each texel ONCE into scratch storage in
// `scratchDirectory` (the system temporary directory when empty) and assembles
// from it; the scratch is removed on every exit. Progress: the shading takes
// [0, 0.9] split by rows, per texel inside each region; assembly takes [0.9, 1].
[[nodiscard]] RegionedBakeResult bakeRegions(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map,
                                             const BakeParams& params, bool udim, RegionSink& sink,
                                             ProgressSink* progress = nullptr,
                                             const CancelToken* cancel = nullptr,
                                             const std::string& scratchDirectory = {});

}  // namespace cyber::bake
