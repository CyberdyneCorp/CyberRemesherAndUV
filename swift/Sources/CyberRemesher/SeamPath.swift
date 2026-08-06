// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI (no swiftc here, and publish-swift.yml only
// runs on swift-v* tags). Written against the `cyber_seam_set_*` /
// `cyber_seam_path_*` block of capi/include/cyber_capi.h, not compiled by
// anything in this repo.
//
// The UV Path tool: place waypoints on the EditMesh and the engine routes a
// least-cost edge path between consecutive ones, biased toward feature-tagged
// and concave (valley) edges so short hops follow the groove instead of cutting
// straight across flat surface.
//
// EDITABLE UNTIL COMMIT: every waypoint can be dragged or deleted while the
// path is pending, and an edit re-routes ONLY the at most two segments that
// touch it — `segmentRevision(_:)` makes that observable, so a viewport redraws
// just the segments that actually changed.
//
// COMMIT / RESUME: `commit(into:)` marks the route into a `SeamSet` — the same
// seam model the freehand Pencil/Erase gestures use, so gesture unwrap and sew
// behave identically — and arms `resumeMarker` at the path's last vertex. The
// returned edge ids are the UNDO RECORD: erasing exactly those restores the
// pre-commit state, and edges that were already seams are never listed, so they
// survive the undo. `dropResumeMarker()` only forgets where to continue; it
// never touches a committed seam.

import CCyberRemesher

/// Per-unit-length routing multipliers. Lower means "cheaper, prefer this
/// edge". Values are clamped to a strictly positive floor by the engine, so no
/// setting can break the router.
public struct SeamCostParams {
    public var flatWeight: Float
    public var featureWeight: Float
    public var concaveWeight: Float
    public var creaseDegrees: Float
    public var minWeight: Float

    /// The engine's own defaults, read back through the ABI.
    public init() {
        var c = CyberSeamPathOptions()
        cyber_default_seam_path_options(&c)
        flatWeight = c.flatWeight
        featureWeight = c.featureWeight
        concaveWeight = c.concaveWeight
        creaseDegrees = c.creaseDegrees
        minWeight = c.minWeight
    }

    var cValue: CyberSeamPathOptions {
        CyberSeamPathOptions(
            flatWeight: flatWeight,
            featureWeight: featureWeight,
            concaveWeight: concaveWeight,
            creaseDegrees: creaseDegrees,
            minWeight: minWeight
        )
    }
}

/// A mutable set of seam edges: the model both routed paths and hand-drawn
/// strokes write into.
public final class SeamSet {
    let handle: OpaquePointer

    public init() throws {
        var out: OpaquePointer?
        try CyberError.check(cyber_seam_set_create(&out))
        guard let handle = out else {
            throw CyberError.runtime("cyber_seam_set_create returned a null handle")
        }
        self.handle = handle
    }

    deinit {
        cyber_seam_set_free(handle)
    }

    /// Number of seam edges.
    public var count: Int {
        Int(cyber_seam_set_count(handle))
    }

    public func isSeam(_ edge: UInt32) -> Bool {
        cyber_seam_set_is_seam(handle, edge) == 1
    }

    /// Seam edge ids in ascending order.
    public func edges() -> [UInt32] {
        let total = Int(cyber_seam_set_edges(handle, nil, 0))
        var out = [UInt32](repeating: 0, count: total)
        guard total > 0 else { return out }
        out.withUnsafeMutableBufferPointer { buffer in
            _ = cyber_seam_set_edges(handle, buffer.baseAddress, buffer.count)
        }
        return out
    }

    public func mark(_ edge: UInt32) throws {
        try CyberError.check(cyber_seam_set_mark(handle, edge))
    }

    public func erase(_ edge: UInt32) throws {
        try CyberError.check(cyber_seam_set_erase(handle, edge))
    }

    /// Undoes a `SeamPath.commit(into:)` from the edge ids it returned.
    public func revertCommit(_ markedEdges: [UInt32]) throws {
        for edge in markedEdges {
            try erase(edge)
        }
    }
}

/// An editable, auto-routed seam path.
///
/// SNAPSHOT SEMANTICS, as for `Snapper`: the path references the mesh it was
/// built on and caches vertex ids against it, so recreate it if that mesh is
/// edited.
public final class SeamPath {
    let handle: OpaquePointer
    /// Kept alive for the path's lifetime: the engine holds a raw reference.
    private let mesh: Mesh

    public init(mesh: Mesh, params: SeamCostParams = SeamCostParams()) throws {
        var options = params.cValue
        var out: OpaquePointer?
        try CyberError.check(cyber_seam_path_create(mesh.handle, &options, &out))
        guard let handle = out else {
            throw CyberError.runtime("cyber_seam_path_create returned a null handle")
        }
        self.handle = handle
        self.mesh = mesh
    }

    deinit {
        cyber_seam_path_free(handle)
    }

    // MARK: - Editing

    /// Appends a waypoint and routes the segment closing onto it. With the
    /// resume marker armed and no waypoints yet, the path is seeded at the
    /// marker first. False for a dead vertex or a repeat of the last waypoint.
    @discardableResult
    public func addWaypoint(_ vertex: UInt32) -> Bool {
        cyber_seam_path_add_waypoint(handle, vertex) == 1
    }

    /// Repositions a waypoint, re-routing ONLY its adjacent segments.
    @discardableResult
    public func moveWaypoint(_ index: Int, to vertex: UInt32) -> Bool {
        cyber_seam_path_move_waypoint(handle, index, vertex) == 1
    }

    /// Drag gesture: repositions a waypoint onto the nearest live vertex within
    /// `radius` of `position`. False (path untouched) when nothing is in range.
    @discardableResult
    public func moveWaypoint(
        _ index: Int, toPosition position: (Float, Float, Float), radius: Float
    ) -> Bool {
        var p = [position.0, position.1, position.2]
        return p.withUnsafeMutableBufferPointer { buffer in
            cyber_seam_path_move_waypoint_to(handle, index, buffer.baseAddress, radius) == 1
        }
    }

    /// Deletes a waypoint; an interior deletion merges its two adjacent
    /// segments into one re-routed segment.
    @discardableResult
    public func removeWaypoint(_ index: Int) -> Bool {
        cyber_seam_path_remove_waypoint(handle, index) == 1
    }

    /// Drops the pending waypoints; committed seams and the marker stay.
    public func clear() {
        cyber_seam_path_clear(handle)
    }

    // MARK: - Pending state

    public var waypointCount: Int {
        Int(cyber_seam_path_waypoint_count(handle))
    }

    public func waypoints() -> [UInt32] {
        readIds { cyber_seam_path_waypoints(handle, $0, $1) }
    }

    public var segmentCount: Int {
        Int(cyber_seam_path_segment_count(handle))
    }

    /// Routed vertex chain of one segment, endpoints inclusive.
    public func segment(_ index: Int) -> [UInt32] {
        readIds { cyber_seam_path_segment(handle, index, $0, $1) }
    }

    /// Counter bumped each time THIS segment re-routes.
    public func segmentRevision(_ index: Int) -> UInt64 {
        cyber_seam_path_segment_revision(handle, index)
    }

    /// True when there is at least one segment and every segment routed.
    public var isRouted: Bool {
        cyber_seam_path_is_routed(handle) == 1
    }

    /// The whole pending path as one vertex chain (shared joints appear once).
    public func vertices() -> [UInt32] {
        readIds { cyber_seam_path_vertices(handle, $0, $1) }
    }

    /// The live edges the pending path would turn into seams.
    public func edges() -> [UInt32] {
        readIds { cyber_seam_path_edges(handle, $0, $1) }
    }

    // MARK: - Commit / resume

    /// Turns the routed path into seams; returns the newly marked edge ids —
    /// the undo record. A path that is not fully routed commits nothing and is
    /// left pending for repair.
    public func commit(into seams: SeamSet) -> [UInt32] {
        // commit MUTATES, so the usual size-query-then-fill would commit twice.
        // The pending edge count is an exact upper bound on the marked ones.
        let capacity = edges().count
        guard capacity > 0 else {
            _ = cyber_seam_path_commit(handle, seams.handle, nil, 0)
            return []
        }
        var out = [UInt32](repeating: 0, count: capacity)
        let total = out.withUnsafeMutableBufferPointer { buffer in
            Int(cyber_seam_path_commit(handle, seams.handle, buffer.baseAddress, buffer.count))
        }
        return Array(out.prefix(min(total, capacity)))
    }

    /// Vertex the last commit ended on, or nil when no marker is armed.
    public var resumeMarker: UInt32? {
        let value = cyber_seam_path_resume_marker(handle)
        return value == CYBER_INVALID_ID ? nil : value
    }

    /// Starts the next path fresh. Never touches a committed seam.
    public func dropResumeMarker() {
        cyber_seam_path_drop_resume_marker(handle)
    }

    // MARK: - Private

    /// copy_positions convention: size query, then fill.
    private func readIds(
        _ reader: (UnsafeMutablePointer<UInt32>?, Int) -> Int
    ) -> [UInt32] {
        let total = reader(nil, 0)
        guard total > 0 else { return [] }
        var out = [UInt32](repeating: 0, count: total)
        out.withUnsafeMutableBufferPointer { buffer in
            _ = reader(buffer.baseAddress, buffer.count)
        }
        return out
    }
}

/// Dihedral angle of a mesh edge in degrees: positive in a concave valley,
/// negative over a convex ridge, 0 when flat or not two-face manifold. This is
/// the curvature term `SeamPath` routes on.
extension Mesh {
    public func edgeSignedDihedral(_ edge: UInt32) -> Float {
        cyber_mesh_edge_signed_dihedral(handle, edge)
    }
}
