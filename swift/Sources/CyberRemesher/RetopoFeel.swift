// What makes the retopology toolset feel right rather than merely work: a relax
// scoped to the edit, loop slide, and interactive symmetry.

import CCyberRemesher

/// A mirror plane and how vertices on it are treated.
///
/// One plane per call: mirroring on X and Y is two calls, which is exactly what
/// it means geometrically.
public struct MirrorPlane: Sendable, Equatable {
    public var origin: SIMD3<Float>
    /// Need not be unit length, but must not be zero.
    public var normal: SIMD3<Float>
    /// How close a vertex must be to count as ON the plane. Center-line vertices
    /// snap onto it and are shared by both halves rather than duplicated.
    public var weldTolerance: Float
    /// The authored half: the one the normal points into (`true`) or away from.
    public var workingSidePositive: Bool

    public init(
        origin: SIMD3<Float> = .zero, normal: SIMD3<Float> = SIMD3(1, 0, 0),
        weldTolerance: Float = 1e-4, workingSidePositive: Bool = true
    ) {
        self.origin = origin
        self.normal = normal
        self.weldTolerance = weldTolerance
        self.workingSidePositive = workingSidePositive
    }

    var cValue: CyberSymmetry {
        CyberSymmetry(
            origin: (origin.x, origin.y, origin.z), normal: (normal.x, normal.y, normal.z),
            weld_tolerance: weldTolerance, working_side_positive: workingSidePositive ? 1 : 0)
    }
}

/// What re-symmetrizing did.
///
/// `unmatched` counts off-side vertices with no mirror counterpart within
/// tolerance: one-sided geometry is left alone rather than forced into a mirror
/// image that does not exist.
public struct ResymmetrizeReport: Sendable, Equatable {
    public let snapped: Int
    public let matched: Int
    public let unmatched: Int
    public let maxCorrection: Float
}

/// What a loop slide did. `moved` can be less than `loopVertices`: a vertex
/// with no quad on the requested side stays put.
public struct LoopSlideReport: Sendable, Equatable {
    public let loopVertices: Int
    public let moved: Int
}

extension Mesh {
    /// Auto Relax, scoped to an edit.
    ///
    /// Relaxes every vertex within `rings` edge hops of `seeds`. Pass the
    /// vertices an edit just produced — `buildFace`'s ring, a contour's
    /// vertices — so new topology settles into its neighbours and nothing
    /// further away moves. Vertices outside the region keep their EXACT
    /// positions.
    ///
    /// The region is topological, not a radius in space: a strip drawn down a
    /// thin limb is long and narrow, and a sphere big enough to cover it would
    /// reach through to the far side of the limb.
    @discardableResult
    public func relaxRegion(
        seeds: [UInt32], rings: Int = 2, strength: Float = 0.5, iterations: Int = 4,
        autoPinCorners: Bool = true, pinned: [UInt32] = [], snapper: Snapper? = nil,
        resnapEpsilon: Float = 0
    ) throws -> SoftTransformReport {
        var report = CyberSoftTransformReport()
        try CyberError.check(
            seeds.withUnsafeBufferPointer { s in
                pinned.withUnsafeBufferPointer { p in
                    cyber_retopo_relax_region(
                        handle, s.baseAddress, seeds.count, Int32(rings), strength,
                        Int32(iterations), autoPinCorners ? 1 : 0, p.baseAddress, pinned.count,
                        snapper?.handle, resnapEpsilon, &report)
                }
            })
        return SoftTransformReport(
            moved: report.moved, resnapped: report.resnapped,
            maxSnapDistance: report.max_snap_distance)
    }

    /// Slide the edge loop through `edge` a fraction `t` along its rails.
    ///
    /// Every vertex moves toward the SAME side — positive `t` one way, negative
    /// the other — so a closed ring slides without twisting. `|t|` must be below
    /// 1: at 1 the loop lands on its neighbour and every rail collapses.
    ///
    /// The loop is the one `edgeLoop(through:)` reports, which continues only
    /// through valence-4 vertices. On an open border that is a single edge.
    @discardableResult
    public func slideLoop(edge: UInt32, t: Float, snapper: Snapper? = nil) throws
        -> LoopSlideReport
    {
        var report = CyberLoopSlideReport()
        try CyberError.check(cyber_retopo_slide_loop(handle, edge, t, snapper?.handle, &report))
        return LoopSlideReport(loopVertices: report.loop_vertex_count, moved: report.moved_count)
    }

    /// Snap every vertex within the weld tolerance exactly onto the plane.
    @discardableResult
    public func snapToSymmetryPlane(_ plane: MirrorPlane) throws -> Int {
        var c = plane.cValue
        var snapped = 0
        try CyberError.check(cyber_retopo_snap_symmetry_plane(handle, &c, &snapped))
        return snapped
    }

    /// Bake the mirror into real geometry; returns the faces added.
    ///
    /// Every face wholly on the working side gains a mirrored twin with reversed
    /// winding. On-plane vertices are shared, so the seam stays manifold.
    @discardableResult
    public func applySymmetry(_ plane: MirrorPlane, snapper: Snapper? = nil) throws -> Int {
        var c = plane.cValue
        var added = 0
        try CyberError.check(cyber_retopo_apply_symmetry(handle, &c, snapper?.handle, &added))
        return added
    }

    /// Mirror the working half onto the other half IN PLACE. Adds and removes
    /// nothing; only the non-working half's positions move.
    @discardableResult
    public func resymmetrize(_ plane: MirrorPlane, matchTolerance: Float = 0) throws
        -> ResymmetrizeReport
    {
        var c = plane.cValue
        var r = CyberResymmetrizeReport()
        try CyberError.check(cyber_retopo_resymmetrize(handle, &c, matchTolerance, &r))
        return ResymmetrizeReport(
            snapped: Int(r.snapped), matched: Int(r.matched), unmatched: Int(r.unmatched),
            maxCorrection: r.max_correction)
    }
}
