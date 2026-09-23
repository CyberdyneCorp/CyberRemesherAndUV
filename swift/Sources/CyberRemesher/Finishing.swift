// UV unwrapping, baking and image output — the half of a retopology app that
// turns a clean mesh into a shippable asset.
//
// Swift could draw topology and not finish it: 0 of 32 `cyber_uv_*`,
// `cyber_bake*` and `cyber_image_*` entry points were bound, so an iPad host
// had to drop to C to unwrap or bake. Python had the mirror-image gap — all of
// this and none of the drawing tools — so between the two bindings the whole
// workflow existed and neither one could run it.
//
// `Image` and `Snapper` own their handles the same way `Mesh` does: `final`,
// not `Sendable`, released in `deinit`.

import CCyberRemesher

// MARK: - UV

/// How the automatic atlas cuts and packs charts.
///
/// The memberwise defaults come from the engine (`cyber_default_atlas_params`),
/// so a freshly initialized value matches what the CLI does.
public struct AtlasParameters: Sendable {
    /// Normal-coherence bound for chart growth.
    public var maxChartAngleDegrees: Float
    /// Gap around each island, in UV units.
    public var packMargin: Float
    /// Resolution used for the texel-density readout — it does not resize anything.
    public var textureSize: Int32
    /// Rotate each chart to its minimum-area orientation before packing.
    public var reorientCharts: Bool
    /// Merge adjacent charts that share a compatible orientation.
    public var mergeCharts: Bool
    /// Looser merge cap: keep merging while distortion stays under this.
    public var maxChartDistortion: Float

    public init() {
        var defaults = CyberAtlasParams()
        cyber_default_atlas_params(&defaults)
        maxChartAngleDegrees = defaults.maxChartAngleDegrees
        packMargin = defaults.packMargin
        textureSize = defaults.textureSize
        reorientCharts = defaults.reorientCharts != 0
        mergeCharts = defaults.mergeCharts != 0
        maxChartDistortion = defaults.maxChartDistortion
    }

    var cValue: CyberAtlasParams {
        CyberAtlasParams(
            maxChartAngleDegrees: maxChartAngleDegrees,
            packMargin: packMargin,
            textureSize: textureSize,
            reorientCharts: reorientCharts ? 1 : 0,
            mergeCharts: mergeCharts ? 1 : 0,
            maxChartDistortion: maxChartDistortion)
    }
}

/// How an unwrap along explicit seams packs the result.
public struct UnwrapSeamsParameters: Sendable {
    public var packMargin: Float
    public var textureSize: Int32
    public var reorientCharts: Bool

    public init() {
        var defaults = CyberUnwrapSeamsParams()
        cyber_default_unwrap_seams_params(&defaults)
        packMargin = defaults.packMargin
        textureSize = defaults.textureSize
        reorientCharts = defaults.reorientCharts != 0
    }

    var cValue: CyberUnwrapSeamsParams {
        CyberUnwrapSeamsParams(
            packMargin: packMargin, textureSize: textureSize,
            reorientCharts: reorientCharts ? 1 : 0)
    }
}

/// What an unwrap produced, and how badly it distorted.
///
/// `flippedCharts` and `droppedCharts` are the two that mean something is
/// wrong rather than merely imperfect: a mirrored chart bakes inside-out, and a
/// dropped one covers no texels at all.
public struct AtlasResult: Sendable, Equatable {
    public let chartCount: Int
    public let seamEdges: Int
    public let maxAngleDistortion: Float
    public let rmsAngleDistortion: Float
    public let flippedCharts: Int
    public let fallbackCharts: Int
    /// Fraction of the unit square the chart geometry covers.
    public let packedArea: Float
    /// Texels per UV unit at the packed scale.
    public let texelDensity: Float
    public let droppedCharts: Int
    /// Fraction covered by the charts' bounding boxes — always >= `packedArea`,
    /// and the gap between them is the padding you are paying for.
    public let packedBoxArea: Float

    init(_ c: CyberAtlasResult) {
        chartCount = Int(c.chartCount)
        seamEdges = c.seamEdges
        maxAngleDistortion = c.maxAngleDistortion
        rmsAngleDistortion = c.rmsAngleDistortion
        flippedCharts = Int(c.flippedCharts)
        fallbackCharts = Int(c.fallbackCharts)
        packedArea = c.packedArea
        texelDensity = c.texelDensity
        droppedCharts = Int(c.droppedCharts)
        packedBoxArea = c.packedBoxArea
    }
}

extension Mesh {
    /// Automatic UV atlas: cut charts, unwrap, pack, in one call.
    @discardableResult
    public func unwrap(_ parameters: AtlasParameters = AtlasParameters()) throws -> AtlasResult {
        var params = parameters.cValue
        var result = CyberAtlasResult()
        try CyberError.check(cyber_uv_atlas(handle, &params, &result))
        return AtlasResult(result)
    }

    /// Unwrap along a seam set the artist drew, rather than seams the engine chose.
    @discardableResult
    public func unwrap(
        seams: SeamSet, parameters: UnwrapSeamsParameters = UnwrapSeamsParameters()
    ) throws -> AtlasResult {
        var params = parameters.cValue
        var result = CyberAtlasResult()
        try CyberError.check(cyber_uv_unwrap_seams(handle, seams.handle, &params, &result))
        return AtlasResult(result)
    }

    /// Remove edges from a seam set and re-stitch the charts they separated.
    public func stitchSeams(_ seams: SeamSet, edges: [UInt32]) throws {
        try CyberError.check(
            edges.withUnsafeBufferPointer {
                cyber_uv_stitch_seams(handle, seams.handle, $0.baseAddress, edges.count)
            })
    }
}

// MARK: - Baking

/// Which map to bake from the high-poly onto the low-poly.
public struct BakeMap: RawRepresentable, Equatable, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    /// Tangent-space normal map (RGB, encoded to [0,1]).
    public static let normal = BakeMap(rawValue: CYBER_BAKE_NORMAL.rawValue)
    /// Ambient occlusion. The engine's field callback answers OPENNESS, not
    /// occlusion — see `FieldEvaluator`; the map itself is what its name says.
    public static let ambientOcclusion = BakeMap(rawValue: CYBER_BAKE_AO.rawValue)
    /// Signed height along the low-poly normal (1 channel).
    public static let displacement = BakeMap(rawValue: CYBER_BAKE_DISPLACEMENT.rawValue)
    /// Target hit position in MODEL UNITS, unencoded (RGB). See
    /// `objectPosition` for the [0,1] encoding over a bounding box.
    public static let position = BakeMap(rawValue: CYBER_BAKE_POSITION.rawValue)
    /// Target vertex colour at the hit (RGB).
    public static let color = BakeMap(rawValue: CYBER_BAKE_COLOR.rawValue)
    /// Signed mean curvature around mid-grey (1 channel).
    public static let curvature = BakeMap(rawValue: CYBER_BAKE_CURVATURE.rawValue)
    /// Concavity only; white is flat or convex (1 channel).
    public static let cavity = BakeMap(rawValue: CYBER_BAKE_CAVITY.rawValue)
    /// Target normal in object space, encoded `n * 0.5 + 0.5` (RGB).
    public static let objectNormal = BakeMap(rawValue: CYBER_BAKE_OBJECT_NORMAL.rawValue)
    /// `position`'s hit point rescaled so the bake bounds span [0,1] (RGB).
    /// Decode it with `Image.encoding`.
    public static let objectPosition = BakeMap(rawValue: CYBER_BAKE_OBJECT_POSITION.rawValue)
    /// Mean unoccluded hemisphere direction, encoded `n * 0.5 + 0.5` (RGB).
    public static let bentNormal = BakeMap(rawValue: CYBER_BAKE_BENT_NORMAL.rawValue)
    /// Material behind the surface, in model units, times `thicknessScale`.
    public static let thickness = BakeMap(rawValue: CYBER_BAKE_THICKNESS.rawValue)
    /// One flat colour per Target `material_id` (RGB). The texels are EXACT
    /// KEYS: compare them at zero tolerance and never filter, resample or
    /// colour-convert the map. Resolve one with `ImageEncoding.idColors`.
    public static let materialId = BakeMap(rawValue: CYBER_BAKE_MATERIAL_ID.rawValue)
    /// One flat colour per Target object or submesh (RGB), from `object_id`,
    /// then `group_id`, then the Target's face-connected components.
    public static let objectId = BakeMap(rawValue: CYBER_BAKE_OBJECT_ID.rawValue)
    /// The Target normal carried into WORLD space by `BakeParameters.placement`
    /// (by its inverse transpose), encoded `n * 0.5 + 0.5` (RGB). With an
    /// identity placement this is bit-identical to `objectNormal`: this engine
    /// has one model space, and the placement is what separates them.
    public static let worldDirection = BakeMap(rawValue: CYBER_BAKE_WORLD_DIRECTION.rawValue)
    /// Texels per SQUARE model unit given by this mesh's UV layout at the
    /// requested resolution, as a single channel. Zero is the sentinel for "no
    /// density here"; no defined density can take it.
    public static let uvDensity = BakeMap(rawValue: CYBER_BAKE_UV_DENSITY.rawValue)
}

/// Axis convention the object-space maps are expressed in.
public enum UpAxis: UInt32, Sendable {
    case y = 0
    case z = 1
}

/// Frame a bent-normal bake is expressed in.
public enum BentNormalSpace: UInt32, Sendable {
    case tangent = 0
    case object = 1
}

/// What the numbers in a baked image mean.
public enum EncodingBasis: UInt32, Sendable {
    /// Raw values: ambient occlusion, colour, curvature, cavity.
    case none = 0
    /// A direction in the texel's tangent frame, `v * 0.5 + 0.5`.
    case tangentNormal = 1
    /// A direction in object space (`upAxis`), `v * 0.5 + 0.5`.
    case objectNormal = 2
    /// A position rescaled over `[boundsMin, boundsMax]`.
    case objectBounds = 3
    /// A length in model units, multiplied by `scale`.
    case distance = 4
    /// An EXACT key, not a measurement. Never filter, resample or
    /// colour-convert such a map; compare its texels at zero tolerance.
    case idColor = 5
    /// A unit direction in WORLD space: the object-space direction carried
    /// through `ImageEncoding.placement`, then the up axis, then `v * 0.5 + 0.5`.
    case worldDirection = 6
    /// Texels per square model unit. The range this encoding guarantees is
    /// `[0, +infinity)` — deliberately NOT `[0,1]`, in either mode.
    case uvDensity = 7
}

/// How a `BakeMap.uvDensity` bake normalizes its values.
public enum DensityNormalization: UInt32, Sendable {
    /// Texels per square model unit as measured — what a scale-locked material
    /// needs.
    case absolute = 0
    /// Each defined texel over the map's own mean — what shows an artist that
    /// one island is packed differently from the rest.
    case relative = 1
}

/// How a `BakeMap.uvDensity` map was normalized, and the mean it measured.
///
/// `mean` is the MEAN ABSOLUTE density of the map's defined texels, in texels
/// per square model unit, and is reported in both modes: it converts a relative
/// map back to an absolute one. Zero when the map defined no texel. Neutral
/// (absolute, mean 0) for every map that is not a density map.
public struct ImageDensity: Sendable, Equatable {
    public let normalization: DensityNormalization
    public let mean: Float

    init(_ c: CyberImageDensity) {
        normalization = DensityNormalization(rawValue: UInt32(bitPattern: c.normalization))
            ?? .absolute
        mean = c.mean
    }
}

/// The 4x4 row-major identity, the default placement.
public let identityPlacement: [Float] = [
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
]

/// Reads a C `float[16]` tuple out as a Swift array, and writes one back. The
/// C fixed array imports as a 16-element tuple, which no literal should have to
/// spell out at a call site.
func placementArray<T>(_ tuple: T) -> [Float] {
    withUnsafeBytes(of: tuple) { raw in
        Array(raw.bindMemory(to: Float.self).prefix(16))
    }
}

/// Writes a 16-float row-major placement into the C `float[16]` tuple.
///
/// A list of any other length is written as the ZERO matrix, which the engine
/// refuses as singular, rather than being padded or truncated into a plausible
/// one. `placement` documents that anything but 16 finite floats whose linear
/// part is invertible is refused rather than folded to the identity, and a
/// caller who dropped the last row must get that refusal -- not a bake against
/// a matrix they never wrote. This is the same outcome the Python binding
/// reaches by raising on the length.
func writePlacement<T>(_ values: [Float], into tuple: inout T) {
    let exact = values.count == 16 ? values : [Float](repeating: 0, count: 16)
    withUnsafeMutableBytes(of: &tuple) { raw in
        exact.withUnsafeBytes { source in
            raw.copyMemory(from: UnsafeRawBufferPointer(rebasing: source.prefix(raw.count)))
        }
    }
}

/// How a map's padded band was filled.
///
/// The rule follows the map's channel semantics, taken from its encoding
/// basis: a direction map is renormalized after extrapolation, an id map is
/// copied verbatim and never interpolated, anything else is extrapolated.
public enum PaddingMode: UInt32, Sendable {
    /// Radius 0, or no covered texel to pad from.
    case none = 0
    /// The nearest covered texel, copied VERBATIM.
    case nearest = 1
    /// The gradient running off the island, continued outward.
    case extrapolate = 2
    /// Continued, then renormalized to unit length.
    case extrapolateUnit = 3
}

/// What the border-padding stage did to a baked map.
public struct ImagePadding: Sendable, Equatable {
    /// The radius applied, in texels; 0 means padding was disabled.
    public let radius: Int32
    public let mode: PaddingMode
    /// Texels the padded band wrote.
    public let texelsFilled: UInt64

    init(_ c: CyberImagePadding) {
        radius = c.radius
        mode = PaddingMode(rawValue: UInt32(bitPattern: c.mode)) ?? .none
        texelsFilled = c.texelsFilled
    }
}

/// One row of an id map's id-to-colour table.
///
/// `color` is the exact 8-bit triple written for `id`; the float in the image
/// is `channel / 255`. No assigned colour is `(0, 0, 0)` — that is reserved
/// for "no id" and is what an uncovered texel holds.
public struct IdColor: Sendable, Equatable {
    public let id: Int32
    public let color: (UInt8, UInt8, UInt8)

    /// Public so host code can build the row it wants to compare a picked
    /// colour against; the memberwise initializer of a struct with `let`
    /// members is internal.
    public init(id: Int32, color: (UInt8, UInt8, UInt8)) {
        self.id = id
        self.color = color
    }

    public static func == (lhs: IdColor, rhs: IdColor) -> Bool {
        lhs.id == rhs.id && lhs.color == rhs.color
    }
}

/// The basis needed to interpret a baked map.
///
/// Decode an `.objectBounds` texel with
/// `boundsMin + value * (boundsMax - boundsMin)`; the box is already expressed
/// in `upAxis`, so no swizzle has to be re-derived.
public struct ImageEncoding: Sendable {
    public let basis: EncodingBasis
    public let upAxis: UpAxis
    public let boundsMin: (Float, Float, Float)
    public let boundsMax: (Float, Float, Float)
    /// The factor a `.distance` map was multiplied by; 1 otherwise.
    public let scale: Float
    /// Which Target column an `.idColor` map read: `"material_id"`,
    /// `"object_id"`, `"group_id"`, `"component"` for the
    /// face-connected-component fallback, or `"none"`. Empty otherwise.
    public let idSource: String
    /// Every distinct Target id and its colour, ascending by id. Empty for
    /// every basis but `.idColor`.
    public let idColors: [IdColor]

    init(_ c: CyberImageEncoding, idSource: String = "", idColors: [IdColor] = []) {
        basis = EncodingBasis(rawValue: UInt32(bitPattern: c.basis)) ?? .none
        upAxis = UpAxis(rawValue: UInt32(bitPattern: c.upAxis)) ?? .y
        boundsMin = (c.boundsMin.0, c.boundsMin.1, c.boundsMin.2)
        boundsMax = (c.boundsMax.0, c.boundsMax.1, c.boundsMax.2)
        scale = c.scale
        self.idSource = idSource
        self.idColors = idColors
    }
}

/// Bake resolution and ray setup.
public struct BakeParameters: Sendable {
    public var width: Int32
    public var height: Int32
    /// Rays start at surface + normal * this, and cast inward twice as far.
    /// Too small and thin features miss; too large and nearby shells bleed.
    public var cageDistance: Float
    /// Hemisphere rays per texel for AO.
    public var aoSamples: Int32
    /// An AO ray hit beyond this does not occlude.
    public var aoRadius: Float
    public var curvatureRange: Float
    /// Axis convention for the object-space maps.
    public var upAxis: UpAxis
    /// Frame `BakeMap.bentNormal` is expressed in.
    public var bentNormalSpace: BentNormalSpace
    /// Factor `BakeMap.thickness` multiplies its mean back-facing depth by.
    /// Finite and >= 0; the default matches ArmorPaint's doubling.
    public var thicknessScale: Float
    /// Texels of border padding grown outward from every UV island before the
    /// map is returned. 0 disables padding; a negative value is refused.
    public var paddingRadius: Int32
    /// The 4x4 ROW-MAJOR object->world matrix `BakeMap.worldDirection` carries
    /// its normals through, by its inverse transpose. 16 finite floats whose
    /// upper-left 3x3 is invertible; anything else is refused rather than folded
    /// to the identity. Read by no other map, and checked only for a map that
    /// reads it -- a bake of any other map is unaffected by what is here.
    public var placement: [Float]
    /// How `BakeMap.uvDensity` normalizes its values.
    public var densityNormalization: DensityNormalization

    public init() {
        let defaults = defaultBakeParams()
        width = defaults.width
        height = defaults.height
        cageDistance = defaults.cageDistance
        aoSamples = defaults.aoSamples
        aoRadius = defaults.aoRadius
        curvatureRange = defaults.curvatureRange
        upAxis = UpAxis(rawValue: UInt32(bitPattern: defaults.upAxis)) ?? .y
        bentNormalSpace =
            BentNormalSpace(rawValue: UInt32(bitPattern: defaults.bentNormalSpace)) ?? .tangent
        thicknessScale = defaults.thicknessScale
        paddingRadius = defaults.paddingRadius
        placement = placementArray(defaults.placement)
        densityNormalization =
            DensityNormalization(rawValue: UInt32(bitPattern: defaults.densityNormalization))
            ?? .absolute
    }

    /// Built member by member from the engine's own defaults rather than with
    /// the memberwise initializer: `CyberBakeParams` is a sized struct that
    /// grows by appending, and a positional initializer stops compiling on
    /// every append while this does not.
    var cValue: CyberBakeParams {
        var out = defaultBakeParams()
        out.width = width
        out.height = height
        out.cageDistance = cageDistance
        out.aoSamples = aoSamples
        out.aoRadius = aoRadius
        out.curvatureRange = curvatureRange
        out.upAxis = Int32(bitPattern: upAxis.rawValue)
        out.bentNormalSpace = Int32(bitPattern: bentNormalSpace.rawValue)
        out.thicknessScale = thicknessScale
        out.paddingRadius = paddingRadius
        writePlacement(placement, into: &out.placement)
        out.densityNormalization = Int32(bitPattern: densityNormalization.rawValue)
        return out
    }
}

/// A `CyberBakeParams` holding the engine defaults. The struct is SIZED, so its
/// size is stated before the library fills it; once it is, the call cannot be
/// refused, and a refusal here would mean the library broke its own contract.
func defaultBakeParams() -> CyberBakeParams {
    var params = CyberBakeParams()
    params.structSize = MemoryLayout<CyberBakeParams>.size
    let status = cyber_default_bake_params(&params)
    precondition(status == CYBER_OK, "cyber_default_bake_params refused a sized struct")
    return params
}

/// A baked float image. Owns its engine handle; released in `deinit`.
public final class Image {
    let handle: OpaquePointer

    init(owning handle: OpaquePointer) { self.handle = handle }
    deinit { cyber_image_free(handle) }

    public var width: Int { Int(cyber_image_width(handle)) }
    public var height: Int { Int(cyber_image_height(handle)) }
    public var channels: Int { Int(cyber_image_channels(handle)) }

    /// What the pixels mean. Every image has one.
    public var encoding: ImageEncoding {
        var out = CyberImageEncoding()
        guard cyber_image_encoding(handle, &out) == CYBER_OK else {
            return ImageEncoding(CyberImageEncoding())
        }
        let source = cyber_image_id_source(handle).map { String(cString: $0) } ?? ""
        var colors: [IdColor] = []
        for index in 0..<cyber_image_id_color_count(handle) {
            var entry = CyberIdColor()
            guard cyber_image_id_color(handle, index, &entry) == CYBER_OK else { continue }
            colors.append(IdColor(id: entry.id, color: (entry.color.0, entry.color.1,
                                                        entry.color.2)))
        }
        return ImageEncoding(out, idSource: source, idColors: colors)
    }

    /// What the border-padding stage did. Every image has a record.
    public var padding: ImagePadding {
        var out = CyberImagePadding()
        guard cyber_image_padding(handle, &out) == CYBER_OK else {
            return ImagePadding(CyberImagePadding())
        }
        return ImagePadding(out)
    }

    /// How a `BakeMap.uvDensity` map was normalized, and the mean it measured.
    /// Every image has a record; a map that is not a density map reports
    /// `.absolute` with a mean of 0.
    public var density: ImageDensity {
        var out = CyberImageDensity()
        guard cyber_image_density(handle, &out) == CYBER_OK else {
            return ImageDensity(CyberImageDensity())
        }
        return ImageDensity(out)
    }

    /// The 4x4 row-major placement this map was baked with — the identity for
    /// every map that does not read one, so a host can always carry a world
    /// direction back into object space with it.
    public var placement: [Float] {
        var out = [Float](repeating: 0, count: 16)
        let status = out.withUnsafeMutableBufferPointer {
            cyber_image_placement(handle, $0.baseAddress)
        }
        return status == CYBER_OK ? out : identityPlacement
    }

    /// Pixels as floats, row-major, `channels` per texel.
    public func pixels() -> [Float] {
        let needed = cyber_image_copy_pixels(handle, nil, 0)
        guard needed > 0 else { return [] }
        var out = [Float](repeating: 0, count: needed)
        let written = out.withUnsafeMutableBufferPointer {
            cyber_image_copy_pixels(handle, $0.baseAddress, needed)
        }
        return Array(out.prefix(written))
    }

    /// Write a PNG. A sandboxed host passes a path inside its own container.
    public func savePNG(to path: String) throws {
        try CyberError.check(cyber_image_save_png(handle, path))
    }
}

extension Mesh {
    /// Bake a map from `high` onto this mesh, which must already be unwrapped.
    ///
    /// The receiver is the LOW-poly target of the bake — it supplies the UVs
    /// the map is rasterized into, so unwrap before calling this.
    public func bake(
        from high: Mesh, map: BakeMap, parameters: BakeParameters = BakeParameters()
    ) throws -> Image {
        var params = parameters.cValue
        var out: OpaquePointer?
        try CyberError.check(
            cyber_bake(handle, high.handle, CyberBakeMap(rawValue: map.rawValue), &params, &out))
        guard let out else { throw CyberError.outOfMemory }
        return Image(owning: out)
    }
}

// MARK: - UDIM-aware baking

/// One tile of a UDIM bake.
public struct UdimTileBake {
    /// The tile number, under the standard `1001 + u + 10*v` numbering.
    public let tile: Int
    public let image: Image
}

/// What a UV layout occupies, answerable WITHOUT baking — a host has to be able
/// to show what it is about to allocate.
public struct UdimLayout: Sendable, Equatable {
    /// Occupied tile numbers, ASCENDING, under the `1001 + u + 10*v` numbering.
    public let tiles: [Int]
    /// Faces carrying a UV coordinate no tile number can address (`u` outside
    /// `[0, 9]`, or a negative `v`). Counted rather than dropped, so a layout in
    /// a convention this numbering cannot express is visible instead of missing.
    public let unaddressableFaces: Int
}

extension Mesh {
    /// The occupied UDIM tiles of this mesh's UV layout, without baking.
    ///
    /// A mesh with no UV layout reports no tiles rather than throwing.
    public func udimTiles() throws -> UdimLayout {
        var count = 0
        var unaddressable: UInt64 = 0
        try CyberError.check(cyber_udim_tiles(handle, nil, 0, &count, &unaddressable))
        guard count > 0 else { return UdimLayout(tiles: [], unaddressableFaces: 0) }
        var numbers = [Int32](repeating: 0, count: count)
        try numbers.withUnsafeMutableBufferPointer {
            try CyberError.check(
                cyber_udim_tiles(handle, $0.baseAddress, count, &count, &unaddressable))
        }
        return UdimLayout(tiles: numbers.map(Int.init),
                          unaddressableFaces: Int(unaddressable))
    }

    /// Bake `map` from `high` once per occupied UDIM tile of this mesh's layout.
    ///
    /// The acceleration structure over `high` is built ONCE and shared by every
    /// tile, so the rays cast for ambient occlusion, bent normal and thickness
    /// see the WHOLE mesh whatever tile is being written — geometry whose UVs
    /// lie in another tile still occludes.
    ///
    /// Every parameter `bake(from:map:parameters:)` validates is validated here
    /// identically, and the host's texel ceiling applies PER TILE and IN
    /// AGGREGATE; a refusal throws, and `CyberError`'s message names which of
    /// the two ceilings it hit.
    public func bakeUdim(
        from high: Mesh, map: BakeMap, parameters: BakeParameters = BakeParameters()
    ) throws -> [UdimTileBake] {
        var params = parameters.cValue
        var refusal: Int32 = 0
        var set: OpaquePointer?
        try CyberError.check(
            cyber_bake_udim(handle, high.handle, CyberBakeMap(rawValue: map.rawValue), &params,
                            &refusal, &set))
        guard let set else { throw CyberError.outOfMemory }
        defer { cyber_udim_bake_free(set) }
        var tiles: [UdimTileBake] = []
        for index in 0..<cyber_udim_bake_count(set) {
            var number: Int32 = 0
            try CyberError.check(cyber_udim_bake_tile(set, index, &number))
            var image: OpaquePointer?
            try CyberError.check(cyber_udim_bake_image(set, index, &image))
            guard let image else { throw CyberError.outOfMemory }
            tiles.append(UdimTileBake(tile: Int(number), image: Image(owning: image)))
        }
        return tiles
    }
}

// MARK: - Bake provider

/// One map this build produces, as the capability query reports it.
///
/// Everything a host needs in order to decide whether it wants the map, and to
/// size the buffer it is written into, without baking it first.
public struct BakeProviderMap: Sendable, Equatable {
    /// The map to request.
    public let map: BakeMap
    /// Stable machine name — the same vocabulary an export preset uses
    /// (`"normal"`, `"ao"`, `"object-position"`, `"material-id"`, …).
    public let name: String
    /// Floats per texel: 1 or 3.
    public let channels: Int
    /// The basis a bake of this map reports UNDER DEFAULT PARAMETERS.
    /// `BakeMap.bentNormal` reports `.objectNormal` when baked in object space,
    /// so `BakeProviderOutput.encoding` is the authority.
    public let encodingBasis: EncodingBasis
    /// `"linear"` or `"srgb"`.
    public let colorSpace: String
    /// True when a field evaluator alone can produce this map.
    public let fieldCapable: Bool

    init(_ c: CyberBakeProviderMap) {
        map = BakeMap(rawValue: UInt32(bitPattern: c.map))
        name = c.name.map { String(cString: $0) } ?? ""
        channels = Int(c.channels)
        encodingBasis = EncodingBasis(rawValue: UInt32(bitPattern: c.encodingBasis)) ?? .none
        colorSpace = c.colorSpace.map { String(cString: $0) } ?? ""
        fieldCapable = c.fieldCapable != 0
    }
}

/// A map produced through the provider: the pixels plus everything needed to
/// interpret them.
public struct BakeProviderOutput: Sendable {
    public let width: Int
    public let height: Int
    public let channels: Int
    /// Row-major, `channels` per texel.
    public let pixels: [Float]
    public let encoding: ImageEncoding
    public let padding: ImagePadding
    /// Texels the UV layout covered.
    public let texelsCovered: Int
    /// 1 = +Y (OpenGL). This engine bakes +Y; reported so a host never has to
    /// assume it or read it off a preset it may not have.
    public let normalGreenPlusY: Int32
    /// How a `BakeMap.uvDensity` map was normalized, and the mean it measured.
    /// Neutral for every other map.
    public let density: ImageDensity
    /// The 4x4 row-major placement the request was given; identity for every
    /// map that does not read one.
    public let placement: [Float]
}

/// Progress and cancellation handed to one provider request. Passed to C as an
/// opaque `user` pointer, which ARC cannot see, so `bakeThroughProvider` keeps
/// it alive across the call with an explicit `withExtendedLifetime`.
private final class BakeProviderCallbacks {
    let progress: ((Float, String) -> Void)?
    let cancel: (() -> Bool)?

    init(progress: ((Float, String) -> Void)?, cancel: (() -> Bool)?) {
        self.progress = progress
        self.cancel = cancel
    }
}

private func bakeProviderProgress(
    _ fraction: Float, _ stage: UnsafePointer<CChar>?, _ user: UnsafeMutableRawPointer?
) {
    guard let user else { return }
    let box = Unmanaged<BakeProviderCallbacks>.fromOpaque(user).takeUnretainedValue()
    box.progress?(fraction, stage.map { String(cString: $0) } ?? "")
}

private func bakeProviderCancel(_ user: UnsafeMutableRawPointer?) -> Int32 {
    guard let user else { return 0 }
    let box = Unmanaged<BakeProviderCallbacks>.fromOpaque(user).takeUnretainedValue()
    return (box.cancel?() ?? false) ? 1 : 0
}

/// The seam an external map consumer drives: ask what this build produces, then
/// request one of those maps.
public enum BakeProvider {
    /// Every map this build produces, in a stable order.
    public static var maps: [BakeProviderMap] {
        var out: [BakeProviderMap] = []
        for index in 0..<cyber_bake_provider_map_count() {
            var entry = CyberBakeProviderMap()
            entry.structSize = MemoryLayout<CyberBakeProviderMap>.size
            guard cyber_bake_provider_map_at(index, &entry) == CYBER_OK else { continue }
            out.append(BakeProviderMap(entry))
        }
        return out
    }

    /// The advertised map called `name`.
    public static func map(named name: String) throws -> BakeProviderMap {
        var entry = CyberBakeProviderMap()
        entry.structSize = MemoryLayout<CyberBakeProviderMap>.size
        try CyberError.check(cyber_bake_provider_find_map(name, &entry))
        return BakeProviderMap(entry)
    }

    /// The advertised names, comma-separated, as a diagnostic lists them.
    /// `fieldOnly` restricts it to the maps a field evaluator alone can produce.
    public static func mapList(fieldOnly: Bool = false) -> String {
        cyber_bake_provider_map_list(fieldOnly ? 1 : 0).map { String(cString: $0) } ?? ""
    }
}

extension Mesh {
    /// Bake a map from `high` onto this mesh through the provider surface.
    ///
    /// The receiver is the LOW-poly target of the bake — it supplies the UVs the
    /// map is rasterized into, so unwrap before calling this.
    ///
    /// Pixels land in a buffer sized from the engine's own report, so nothing is
    /// guessed at. A cancelled request throws `CyberError.cancelled` and hands
    /// back no pixels; a map outside the advertised set throws naming the map
    /// and the set, rather than returning a neutral image.
    public func bakeThroughProvider(
        from high: Mesh, map: BakeMap, parameters: BakeParameters = BakeParameters(),
        maxIdColors: Int = 4096,
        progress: ((Float, String) -> Void)? = nil,
        cancel: (() -> Bool)? = nil
    ) throws -> BakeProviderOutput {
        var params = parameters.cValue
        let callbacks = BakeProviderCallbacks(progress: progress, cancel: cancel)
        let box = Unmanaged.passUnretained(callbacks).toOpaque()

        var request = CyberBakeProviderRequest()
        request.structSize = MemoryLayout<CyberBakeProviderRequest>.size
        request.low = handle
        request.high = high.handle
        request.map = Int32(bitPattern: map.rawValue)
        request.user = box
        // Always installed: the trampolines read the box, which holds nil for
        // whichever the caller left out. A conditional here would have to be an
        // `if`, because Swift forms a C function pointer only from a direct
        // reference to a `func`, and the branch buys nothing.
        request.progress = bakeProviderProgress
        request.cancel = bakeProviderCancel

        let output = try withUnsafeMutablePointer(to: &params) { paramsPtr in
            request.params = UnsafePointer(paramsPtr)

            // Size first, then allocate exactly what the engine asked for. The
            // sizing call validates the whole request and casts no ray.
            var sizing = CyberBakeProviderResult()
            sizing.structSize = MemoryLayout<CyberBakeProviderResult>.size
            try CyberError.check(cyber_bake_provider_bake(&request, &sizing))

            var pixels = [Float](repeating: 0, count: Int(sizing.pixelCount))
            var idRows = [CyberIdColor](repeating: CyberIdColor(), count: max(1, maxIdColors))
            var result = CyberBakeProviderResult()
            result.structSize = MemoryLayout<CyberBakeProviderResult>.size

            try pixels.withUnsafeMutableBufferPointer { pixelBuffer in
                try idRows.withUnsafeMutableBufferPointer { idBuffer in
                    request.pixels = pixelBuffer.baseAddress
                    request.pixelCapacity = pixelBuffer.count
                    request.idColors = idBuffer.baseAddress
                    request.idColorCapacity = idBuffer.count
                    try CyberError.check(cyber_bake_provider_bake(&request, &result))
                }
            }

            let source = result.idSource.map { String(cString: $0) } ?? ""
            let rows = (0..<min(Int(result.idColorCount), idRows.count)).map { index in
                IdColor(id: idRows[index].id,
                        color: (idRows[index].color.0, idRows[index].color.1,
                                idRows[index].color.2))
            }
            return BakeProviderOutput(
                width: Int(result.width), height: Int(result.height),
                channels: Int(result.channels), pixels: pixels,
                encoding: ImageEncoding(result.encoding, idSource: source, idColors: rows),
                padding: ImagePadding(result.padding),
                texelsCovered: Int(result.texelsCovered),
                normalGreenPlusY: result.normalGreenPlusY,
                density: ImageDensity(result.density),
                placement: placementArray(result.placement))
        }
        // `callbacks` is reachable only through an UNMANAGED opaque pointer for
        // the whole call, so ARC sees its last use at `passUnretained` above and
        // may release it while the engine is still calling the trampolines --
        // which would then resolve a freed object. Same guard, and the same
        // reason, as ZRemesher.swift and Remesh.swift.
        withExtendedLifetime(callbacks) {}
        return output
    }
}
