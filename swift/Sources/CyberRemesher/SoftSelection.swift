// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the `cyber_retopo_selection_*`
// block of capi/include/cyber_capi.h, not compiled by anything in this repo.
//
// Soft selection: a per-vertex weight field in [0,1] living on the mesh handle,
// filled from a line gradient / sphere / paint strokes, reshaped by the
// selection ops, and consumed by the weighted transform and relax.
//
// SURFACE GLUE: passing a `snapper` to `transformSelection` / `relaxSelection`
// re-projects the moved vertices onto the Target INSIDE the same call. There is
// no separate snap pass and none is wanted — running snap-all afterwards would
// also drag the vertices the selection deliberately left alone. A drag gesture
// is driven by calling `transformSelection` once per incremental delta.
//
// ZERO WEIGHT MEANS UNTOUCHED: a vertex at weight 0 is neither moved nor
// re-snapped, so its position is bit-identical after the call.

import CCyberRemesher

/// Falloff curves for a gradient region. All map 0 -> 0 and 1 -> 1
/// monotonically, so the curve changes how a gradient feels, not where it
/// starts or ends.
public enum SoftFalloff: Int32 {
    case linear = 0
    /// Smoothstep — the default gradient feel.
    case smooth = 1
    /// `t^2`: weight builds late.
    case sharp = 2
    /// Quarter circle: weight builds early.
    case round = 3
}

/// A line gradient region: weight ramps 0 at `anchor` to 1 at `end`, and stays
/// 1 beyond `end`.
public struct LineRegion {
    public var anchor: (Float, Float, Float)
    public var end: (Float, Float, Float)
    /// View direction defining the plane the angle snap is measured in — the
    /// same screen-space convention Draw Strip and Surface Cut use.
    public var viewDirection: (Float, Float, Float)
    /// Quantize the anchor->end direction to `snapDegrees` increments.
    public var snapAngle: Bool
    public var snapDegrees: Float
    public var falloff: SoftFalloff

    public init(
        anchor: (Float, Float, Float),
        end: (Float, Float, Float),
        viewDirection: (Float, Float, Float) = (0, 0, 1),
        snapAngle: Bool = false,
        snapDegrees: Float = 15,
        falloff: SoftFalloff = .smooth
    ) {
        self.anchor = anchor
        self.end = end
        self.viewDirection = viewDirection
        self.snapAngle = snapAngle
        self.snapDegrees = snapDegrees
        self.falloff = falloff
    }
}

/// A sphere region: weight 1 at `center` falling to 0 at `radius`.
public struct SphereRegion {
    public var center: (Float, Float, Float)
    public var radius: Float
    public var falloff: SoftFalloff

    public init(
        center: (Float, Float, Float), radius: Float, falloff: SoftFalloff = .smooth
    ) {
        self.center = center
        self.radius = radius
        self.falloff = falloff
    }
}

/// One brush sample along a paint gesture.
public struct PaintSample {
    public var position: (Float, Float, Float)
    public var pressure: Float

    public init(position: (Float, Float, Float), pressure: Float = 1) {
        self.position = position
        self.pressure = pressure
    }
}

/// Outcome of a weighted transform or relax.
///
/// Both counts are DISTINCT VERTICES, never per-iteration writes: `moved` never
/// exceeds the mesh's vertex count even for a multi-iteration `relaxSelection`
/// that revisits each of them every sweep, and `resnapped` never exceeds
/// `moved`.
public struct SoftTransformReport: Equatable {
    /// Distinct vertices the operation wrote (weight > 0 and not pinned).
    public var moved: Int
    /// Of those, how many the Target re-projection pulled back STRICTLY further
    /// than the caller's epsilon. With the default epsilon of 0 a vertex the
    /// projection did not have to correct at all (correction exactly 0 — it
    /// already sat on the Target, which is what happens when the weight is so
    /// small that the blended target is bit-identical to the current position)
    /// is not counted, so `moved - resnapped` is the number of vertices that
    /// were already on-surface.
    public var resnapped: Int
    /// Largest counted correction.
    public var maxSnapDistance: Float
}

extension Mesh {

    // MARK: - Gradient region selection

    /// Replaces the selection with a line gradient.
    public func selectLine(_ region: LineRegion) throws {
        var anchor = [region.anchor.0, region.anchor.1, region.anchor.2]
        var end = [region.end.0, region.end.1, region.end.2]
        var view = [region.viewDirection.0, region.viewDirection.1, region.viewDirection.2]
        try CyberError.check(
            cyber_retopo_selection_line(
                handle, &anchor, &end, &view,
                region.snapAngle ? 1 : 0, region.snapDegrees,
                CyberFalloff(rawValue: UInt32(region.falloff.rawValue))
            )
        )
    }

    /// Replaces the selection with a radial falloff.
    public func selectSphere(_ region: SphereRegion) throws {
        var center = [region.center.0, region.center.1, region.center.2]
        try CyberError.check(
            cyber_retopo_selection_sphere(
                handle, &center, region.radius,
                CyberFalloff(rawValue: UInt32(region.falloff.rawValue))
            )
        )
    }

    /// Accumulates one brush dab into the selection.
    public func paintSelection(
        at position: (Float, Float, Float),
        radius: Float,
        pressure: Float = 1,
        subtract: Bool = false,
        falloff: SoftFalloff = .smooth
    ) throws {
        var center = [position.0, position.1, position.2]
        try CyberError.check(
            cyber_retopo_selection_paint(
                handle, &center, radius, pressure, subtract ? 1 : 0,
                CyberFalloff(rawValue: UInt32(falloff.rawValue))
            )
        )
    }

    /// Accumulates a whole gesture in one call.
    public func paintSelectionStroke(
        _ samples: [PaintSample],
        radius: Float,
        subtract: Bool = false,
        falloff: SoftFalloff = .smooth
    ) throws {
        var flat: [Float] = []
        flat.reserveCapacity(samples.count * 4)
        for sample in samples {
            flat.append(sample.position.0)
            flat.append(sample.position.1)
            flat.append(sample.position.2)
            flat.append(sample.pressure)
        }
        try CyberError.check(
            flat.withUnsafeBufferPointer { buffer in
                cyber_retopo_selection_paint_stroke(
                    handle, buffer.baseAddress, samples.count, radius, subtract ? 1 : 0,
                    CyberFalloff(rawValue: UInt32(falloff.rawValue))
                )
            }
        )
    }

    // MARK: - Selection operations

    /// Zeroes every weight.
    public func clearSelection() throws {
        try CyberError.check(cyber_retopo_selection_clear(handle))
    }

    /// `w -> 1 - w` over the live vertices.
    public func invertSelection() throws {
        try CyberError.check(cyber_retopo_selection_invert(handle))
    }

    /// Grows the selection by the one-ring maximum, `steps` times.
    public func expandSelection(steps: Int32 = 1) throws {
        try CyberError.check(cyber_retopo_selection_expand(handle, steps))
    }

    /// Shrinks the selection by the one-ring minimum, `steps` times.
    public func contractSelection(steps: Int32 = 1) throws {
        try CyberError.check(cyber_retopo_selection_contract(handle, steps))
    }

    /// Softens the weight transition (the shell's smooth by 1 / 5 / 10).
    public func smoothSelection(steps: Int32 = 1) throws {
        try CyberError.check(cyber_retopo_selection_smooth(handle, steps))
    }

    // MARK: - Weight field and named slots

    /// The weight field, indexed by vertex id (a snapshot copy).
    public func selectionWeights() -> [Float] {
        let count = Int(cyber_retopo_selection_copy_weights(handle, nil, 0))
        guard count > 0 else { return [] }
        return [Float](unsafeUninitializedCapacity: count) { buffer, initialized in
            initialized = Int(
                cyber_retopo_selection_copy_weights(handle, buffer.baseAddress, buffer.count)
            )
        }
    }

    /// Replaces the whole weight field; values are clamped into [0, 1].
    public func setSelectionWeights(_ weights: [Float]) throws {
        try CyberError.check(
            weights.withUnsafeBufferPointer { buffer in
                cyber_retopo_selection_set_weights(handle, buffer.baseAddress, buffer.count)
            }
        )
    }

    /// Stores the current selection in the named slot on this handle.
    public func saveSelection(named name: String) throws {
        try CyberError.check(cyber_retopo_selection_save(handle, name))
    }

    /// Restores the selection from the named slot.
    public func loadSelection(named name: String) throws {
        try CyberError.check(cyber_retopo_selection_load(handle, name))
    }

    /// Names of the saved selection slots, in name order.
    public func selectionSlots() -> [String] {
        let count = Int(cyber_retopo_selection_slot_count(handle))
        return (0..<count).compactMap { index in
            guard let raw = cyber_retopo_selection_slot_name(handle, index) else { return nil }
            return String(cString: raw)
        }
    }

    // MARK: - Weighted transform and relax

    /// Applies a 12-float affine (column-major 3x3 then translation) per vertex
    /// scaled by its selection weight, re-snapping to the Target in the same
    /// pass when `snapper` is supplied.
    @discardableResult
    public func transformSelection(
        _ transform: [Float],
        snapper: OpaquePointer? = nil,
        resnapEpsilon: Float = 0
    ) throws -> SoftTransformReport {
        guard transform.count == 12 else {
            throw CyberError.invalidArgument("transform must be 12 floats")
        }
        var report = CyberSoftTransformReport()
        try CyberError.check(
            transform.withUnsafeBufferPointer { buffer in
                cyber_retopo_selection_transform(
                    handle, buffer.baseAddress, snapper, resnapEpsilon, &report
                )
            }
        )
        return SoftTransformReport(
            moved: Int(report.moved),
            resnapped: Int(report.resnapped),
            maxSnapDistance: report.max_snap_distance
        )
    }

    /// Relaxes the mesh with the per-vertex blend scaled by the weights.
    @discardableResult
    public func relaxSelection(
        strength: Float = 0.5,
        iterations: Int32 = 1,
        pinned: [UInt32] = [],
        snapper: OpaquePointer? = nil,
        resnapEpsilon: Float = 0
    ) throws -> SoftTransformReport {
        var report = CyberSoftTransformReport()
        try CyberError.check(
            pinned.withUnsafeBufferPointer { pins in
                cyber_retopo_selection_relax(
                    handle, strength, iterations, pins.baseAddress, pinned.count,
                    snapper, resnapEpsilon, &report
                )
            }
        )
        return SoftTransformReport(
            moved: Int(report.moved),
            resnapped: Int(report.resnapped),
            maxSnapDistance: report.max_snap_distance
        )
    }
}
