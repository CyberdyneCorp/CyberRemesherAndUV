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
    /// Target hit position in world space (RGB).
    public static let position = BakeMap(rawValue: CYBER_BAKE_POSITION.rawValue)
    /// Target vertex colour at the hit (RGB).
    public static let color = BakeMap(rawValue: CYBER_BAKE_COLOR.rawValue)
    /// Signed mean curvature around mid-grey (1 channel).
    public static let curvature = BakeMap(rawValue: CYBER_BAKE_CURVATURE.rawValue)
    /// Concavity only; white is flat or convex (1 channel).
    public static let cavity = BakeMap(rawValue: CYBER_BAKE_CAVITY.rawValue)
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

    public init() {
        var defaults = CyberBakeParams()
        cyber_default_bake_params(&defaults)
        width = defaults.width
        height = defaults.height
        cageDistance = defaults.cageDistance
        aoSamples = defaults.aoSamples
        aoRadius = defaults.aoRadius
        curvatureRange = defaults.curvatureRange
    }

    var cValue: CyberBakeParams {
        CyberBakeParams(
            width: width, height: height, cageDistance: cageDistance,
            aoSamples: aoSamples, aoRadius: aoRadius, curvatureRange: curvatureRange)
    }
}

/// A baked float image. Owns its engine handle; released in `deinit`.
public final class Image {
    let handle: OpaquePointer

    init(owning handle: OpaquePointer) { self.handle = handle }
    deinit { cyber_image_free(handle) }

    public var width: Int { Int(cyber_image_width(handle)) }
    public var height: Int { Int(cyber_image_height(handle)) }
    public var channels: Int { Int(cyber_image_channels(handle)) }

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
