// The interactive retopology verbs, on `Mesh`.
//
// Swift already held the complete stroke grammar (`StrokeInterpretation`) and
// none of the operations that apply what it recognises: a host could classify a
// gesture as `.createQuad` and then find no way to create the quad. These are
// those operations — the ones an embedding app drives from a recognised stroke
// or a direct tool tap.
//
// Every builder that introduces geometry takes an optional `Snapper`. Passing
// one makes new vertices land ON the Target, which is what distinguishes
// retopology from modelling in mid-air; passing `nil` places them exactly where
// the caller asked.

import CCyberRemesher

/// What a contour run produced.
public struct ContourReport: Equatable {
    public let ringCount: Int
    public let faceCount: Int
    public let vertexCount: Int
}

/// Measurements of the edge loop through a seed edge — the Loop Info readout.
///
/// `snappedVertexCount` and `maxSnapDistance` are measured only when a
/// ``Snapper`` was supplied; `snapMeasured` says whether they mean anything,
/// so a loop nobody measured is distinguishable from one measured as unsnapped.
public struct LoopMetrics: Equatable {
    public let edgeCount: Int
    public let vertexCount: Int
    public let length: Float
    public let isClosed: Bool
    public let boundaryEdgeCount: Int
    /// Terminal vertices of an open chain; `nil` for a closed loop.
    public let endpoints: (a: UInt32, b: UInt32)?
    public let snapMeasured: Bool
    public let snappedVertexCount: Int
    public let maxSnapDistance: Float

    public static func == (lhs: LoopMetrics, rhs: LoopMetrics) -> Bool {
        lhs.edgeCount == rhs.edgeCount && lhs.vertexCount == rhs.vertexCount
            && lhs.length == rhs.length && lhs.isClosed == rhs.isClosed
            && lhs.boundaryEdgeCount == rhs.boundaryEdgeCount
            && lhs.endpoints?.a == rhs.endpoints?.a && lhs.endpoints?.b == rhs.endpoints?.b
            && lhs.snapMeasured == rhs.snapMeasured
            && lhs.snappedVertexCount == rhs.snappedVertexCount
            && lhs.maxSnapDistance == rhs.maxSnapDistance
    }
}

extension Mesh {

    // MARK: - Building

    /// Creates one face from 3 or 4 ring positions, snapping them to the Target
    /// first when `snapper` is given. The gesture behind `.createQuad`.
    @discardableResult
    public func createFace(points: [SIMD3<Float>], snapper: Snapper? = nil) throws -> UInt32 {
        var face: UInt32 = 0
        let flat = flatten(points)
        try CyberError.check(
            flat.withUnsafeBufferPointer { p in
                cyber_retopo_create_face(
                    handle, p.baseAddress, points.count, snapper?.handle, &face)
            })
        return face
    }

    /// Builds a face from a ring that mixes EXISTING vertex ids with new
    /// positions — PolyPen, where some corners weld onto what is already there.
    ///
    /// A `nil` entry in `vertices` takes its position from the matching entry
    /// in `points`; a non-`nil` entry reuses that vertex and ignores the point.
    /// Returns the final ring, existing and newly created alike, so a caller
    /// can chain the next face onto it.
    @discardableResult
    public func buildFace(
        vertices: [UInt32?], points: [SIMD3<Float>], snapper: Snapper? = nil
    ) throws -> (face: UInt32, ring: [UInt32]) {
        let count = vertices.count
        guard count == 3 || count == 4, points.count == count else {
            throw CyberError.invalidArgument(
                "buildFace needs 3 or 4 slots and one point per slot")
        }
        // CYBER_BUILD_NEW_VERTEX is the sentinel that means "create one here".
        let ids = vertices.map { $0 ?? CYBER_BUILD_NEW_VERTEX }
        let flat = flatten(points)
        var face: UInt32 = 0
        var ring = [UInt32](repeating: 0, count: count)
        try CyberError.check(
            ids.withUnsafeBufferPointer { v in
                flat.withUnsafeBufferPointer { p in
                    ring.withUnsafeMutableBufferPointer { r in
                        cyber_retopo_build_face(
                            handle, count, v.baseAddress, p.baseAddress, snapper?.handle,
                            &face, r.baseAddress)
                    }
                }
            })
        return (face, ring)
    }

    /// PolyStrips: a quad strip welded onto the boundary edge `startA`–`startB`
    /// whose stations follow `path`.
    ///
    /// `path` is world-space stroke samples, normally resampled at quad-size
    /// arc length. `viewDirection` is the camera forward the stroke was drawn
    /// with — it decides which way the strip's width runs. Returns the number
    /// of new faces.
    @discardableResult
    public func drawStrip(
        path: [SIMD3<Float>],
        width: Float,
        viewDirection: SIMD3<Float>,
        startA: UInt32,
        startB: UInt32,
        snapper: Snapper? = nil
    ) throws -> Int {
        var newFaces = 0
        let flat = flatten(path)
        try CyberError.check(
            flat.withUnsafeBufferPointer { p in
                withUnsafeXYZ(viewDirection) { v in
                    cyber_retopo_draw_strip(
                        handle, p.baseAddress, path.count, width, v, startA, startB,
                        snapper?.handle, &newFaces)
                }
            })
        return newFaces
    }

    /// Contours: sample `target` with cross-section strokes and loft the rings
    /// into a quad tube.
    ///
    /// The opposite construction to ``drawStrip(path:width:viewDirection:startA:startB:snapper:)``.
    /// There the stroke is the ribbon's spine; here each stroke names a cutting
    /// plane and the ring is the Target's cross-section, so four short arcs
    /// drawn down an arm produce a closed tube — including the far side the
    /// artist never drew on.
    ///
    /// Strokes are lofted in the order given. A stroke that names no usable
    /// cross-section fails the whole call with the mesh unchanged.
    @discardableResult
    public func contours(
        target: Mesh,
        strokes: [[SIMD3<Float>]],
        spans: Int,
        snapper: Snapper? = nil,
        closed: Bool = false
    ) throws -> ContourReport {
        var flat = [Float]()
        var offsets = [Int](repeating: 0, count: strokes.count + 1)
        for (i, stroke) in strokes.enumerated() {
            offsets[i + 1] = offsets[i] + stroke.count
            flat.append(contentsOf: flatten(stroke))
        }
        var report = CyberContourReport()
        try CyberError.check(
            flat.withUnsafeBufferPointer { p in
                offsets.withUnsafeBufferPointer { o in
                    cyber_retopo_contours(
                        handle, target.handle, p.baseAddress, o.baseAddress, strokes.count,
                        spans, snapper?.handle, closed ? 1 : 0, &report)
                }
            })
        return ContourReport(
            ringCount: report.ring_count, faceCount: report.face_count,
            vertexCount: report.vertex_count)
    }

    /// A band of quads between two equal-length boundary vertex sequences —
    /// the bridge gesture the stroke grammar recognises.
    @discardableResult
    public func bridge(_ loopA: [UInt32], _ loopB: [UInt32]) throws -> Int {
        guard loopA.count == loopB.count else {
            throw CyberError.invalidArgument("bridge needs two loops of equal length")
        }
        var newFaces = 0
        try CyberError.check(
            loopA.withUnsafeBufferPointer { a in
                loopB.withUnsafeBufferPointer { b in
                    cyber_retopo_bridge_loops(
                        handle, a.baseAddress, b.baseAddress, loopA.count, &newFaces)
                }
            })
        return newFaces
    }

    /// A connected block of quads over a row-major lattice of
    /// `(rows + 1) * (cols + 1)` new vertices — the one-stroke grid gesture.
    @discardableResult
    public func createGrid(
        points: [SIMD3<Float>], rows: Int, cols: Int, snapper: Snapper? = nil
    ) throws -> Int {
        var faces = 0
        let flat = flatten(points)
        try CyberError.check(
            flat.withUnsafeBufferPointer { p in
                cyber_retopo_create_grid(
                    handle, p.baseAddress, rows, cols, snapper?.handle, &faces)
            })
        return faces
    }

    /// Extends an ordered boundary chain by `offset` in `rings` rows of quads,
    /// returning the new outer chain so the next extrusion can continue from it.
    @discardableResult
    public func extendBoundary(
        chain: [UInt32], closed: Bool, offset: SIMD3<Float>, rings: Int = 1,
        snapper: Snapper? = nil
    ) throws -> (outerChain: [UInt32], newFaces: Int) {
        var outer = [UInt32](repeating: 0, count: chain.count)
        var newFaces = 0
        try CyberError.check(
            chain.withUnsafeBufferPointer { c in
                withUnsafeXYZ(offset) { o in
                    outer.withUnsafeMutableBufferPointer { out in
                        cyber_retopo_extend_boundary_grid(
                            handle, c.baseAddress, chain.count, closed ? 1 : 0, o,
                            Int32(rings), snapper?.handle, out.baseAddress, &newFaces)
                    }
                }
            })
        return (outer, newFaces)
    }

    /// Closes an ordered boundary chain with a triangle fan to a new apex.
    @discardableResult
    public func fanBoundary(
        chain: [UInt32], closed: Bool, apexOffset: SIMD3<Float>, snapper: Snapper? = nil
    ) throws -> (apex: UInt32, newFaces: Int) {
        var apex: UInt32 = 0
        var newFaces = 0
        try CyberError.check(
            chain.withUnsafeBufferPointer { c in
                withUnsafeXYZ(apexOffset) { o in
                    cyber_retopo_extend_boundary_fan(
                        handle, c.baseAddress, chain.count, closed ? 1 : 0, o,
                        snapper?.handle, &apex, &newFaces)
                }
            })
        return (apex, newFaces)
    }

    /// Knife-cuts new edges across faces along the segment `a`–`b`.
    @discardableResult
    public func surfaceCut(
        from a: SIMD3<Float>, to b: SIMD3<Float>, viewDirection: SIMD3<Float>,
        triangulateNGons: Bool = false, snapper: Snapper? = nil
    ) throws -> (splitEdges: Int, splitFaces: Int) {
        var edges = 0
        var faces = 0
        try CyberError.check(
            withUnsafeXYZ(a) { pa in
                withUnsafeXYZ(b) { pb in
                    withUnsafeXYZ(viewDirection) { v in
                        cyber_retopo_surface_cut(
                            handle, pa, pb, v, triangulateNGons ? 1 : 0, snapper?.handle,
                            &edges, &faces)
                    }
                }
            })
        return (edges, faces)
    }

    /// Duplicates `faces` under a 3x4 column-major affine, optionally flipping
    /// winding — Patch Clone.
    @discardableResult
    public func patchClone(
        faces: [UInt32], transform: [Float], flip: Bool = false, snapper: Snapper? = nil
    ) throws -> [UInt32] {
        guard transform.count == 12 else {
            throw CyberError.invalidArgument("patchClone needs a 12-float affine")
        }
        var out = [UInt32](repeating: 0, count: faces.count)
        var count = 0
        try CyberError.check(
            faces.withUnsafeBufferPointer { f in
                transform.withUnsafeBufferPointer { xf in
                    out.withUnsafeMutableBufferPointer { o in
                        cyber_retopo_patch_clone(
                            handle, f.baseAddress, faces.count, xf.baseAddress, flip ? 1 : 0,
                            snapper?.handle, o.baseAddress, &count)
                    }
                }
            })
        return Array(out.prefix(count))
    }

    // MARK: - Editing flow

    /// Inserts an edge loop through `edge` at parameter `t`.
    @discardableResult
    public func insertLoop(edge: UInt32, at t: Float = 0.5) throws -> Int {
        var newFaces = 0
        try CyberError.check(cyber_retopo_insert_loop(handle, edge, t, &newFaces))
        return newFaces
    }

    /// Dissolves interior edges, merging each edge's two faces into one.
    @discardableResult
    public func dissolveEdges(_ edges: [UInt32]) throws -> Int {
        var dissolved = 0
        try CyberError.check(
            edges.withUnsafeBufferPointer { e in
                cyber_retopo_dissolve_edges(handle, e.baseAddress, edges.count, &dissolved)
            })
        return dissolved
    }

    /// Rotates an interior edge, turning the local loop-flow direction.
    public func rotateEdge(_ edge: UInt32) throws {
        try CyberError.check(cyber_retopo_rotate_edge(handle, edge))
    }

    /// Merges `remove` into `keep`; `atMidpoint` places the survivor between
    /// them instead of at `keep`.
    public func mergeVertices(keep: UInt32, remove: UInt32, atMidpoint: Bool = false) throws {
        try CyberError.check(
            cyber_retopo_merge_vertices(handle, keep, remove, atMidpoint ? 1 : 0))
    }

    /// Deletes faces, returning how many were removed.
    @discardableResult
    public func deleteFaces(_ faces: [UInt32]) throws -> Int {
        var removed = 0
        try CyberError.check(
            faces.withUnsafeBufferPointer { f in
                cyber_retopo_delete_faces(handle, f.baseAddress, faces.count, &removed)
            })
        return removed
    }

    /// Drops a live vertex at `position`, snapped to the Target when a snapper
    /// is given. Tweak ignores pins by design — a pinned vertex stays movable
    /// by an explicit tweak.
    public func tweakVertex(_ vertex: UInt32, to position: SIMD3<Float>, snapper: Snapper? = nil)
        throws
    {
        try CyberError.check(
            withUnsafeXYZ(position) { p in
                cyber_retopo_tweak_vertex(handle, vertex, p, snapper?.handle)
            })
    }

    // MARK: - Whole-mesh

    /// Projects every unpinned vertex onto the Target.
    @discardableResult
    public func snapAll(to snapper: Snapper, pinned: [UInt32] = []) throws -> (
        moved: Int, maxDistance: Float
    ) {
        var moved = 0
        var maxDistance: Float = 0
        try CyberError.check(
            pinned.withUnsafeBufferPointer { p in
                cyber_retopo_snap_all(
                    handle, snapper.handle, p.baseAddress, pinned.count, &moved, &maxDistance)
            })
        return (moved, maxDistance)
    }

    /// Tangential Laplacian smoothing inside a brush; a radius of 0 or less
    /// relaxes the whole mesh. Pinned vertices do not move.
    public func relax(
        center: SIMD3<Float>, radius: Float, strength: Float = 0.5, iterations: Int = 1,
        autoPinCorners: Bool = true, pinned: [UInt32] = [], snapper: Snapper? = nil
    ) throws {
        try CyberError.check(
            withUnsafeXYZ(center) { c in
                pinned.withUnsafeBufferPointer { p in
                    cyber_retopo_relax(
                        handle, c, radius, strength, Int32(iterations),
                        autoPinCorners ? 1 : 0, p.baseAddress, pinned.count, snapper?.handle)
                }
            })
    }

    // MARK: - Queries

    /// The edge loop through `edge`, as edge ids.
    public func edgeLoop(through edge: UInt32) -> [UInt32] {
        let needed = cyber_mesh_edge_loop(handle, edge, nil, 0)
        guard needed > 0 else { return [] }
        var out = [UInt32](repeating: 0, count: needed)
        let written = out.withUnsafeMutableBufferPointer {
            cyber_mesh_edge_loop(handle, edge, $0.baseAddress, needed)
        }
        return Array(out.prefix(written))
    }

    /// The boundary loop through `edge`, as ordered vertex ids, and whether it
    /// closes.
    public func boundaryLoop(through edge: UInt32) -> (vertices: [UInt32], isClosed: Bool) {
        var closed: Int32 = 0
        let needed = cyber_mesh_boundary_loop(handle, edge, nil, 0, &closed)
        guard needed > 0 else { return ([], false) }
        var out = [UInt32](repeating: 0, count: needed)
        let written = out.withUnsafeMutableBufferPointer {
            cyber_mesh_boundary_loop(handle, edge, $0.baseAddress, needed, &closed)
        }
        return (Array(out.prefix(written)), closed != 0)
    }

    /// Loop Info: what the loop under the cursor is made of, and whether it
    /// actually sits on the Target.
    public func loopMetrics(through edge: UInt32, snapper: Snapper? = nil) throws -> LoopMetrics {
        var m = CyberLoopMetrics()
        try CyberError.check(cyber_mesh_loop_metrics(handle, edge, snapper?.handle, &m))
        return LoopMetrics(
            edgeCount: Int(m.edge_count), vertexCount: Int(m.vertex_count), length: m.length,
            isClosed: m.closed != 0, boundaryEdgeCount: Int(m.boundary_edge_count),
            endpoints: m.has_endpoints != 0 ? (m.endpoint_a, m.endpoint_b) : nil,
            snapMeasured: m.snap_measured != 0,
            snappedVertexCount: Int(m.snapped_vertex_count), maxSnapDistance: m.max_snap_distance)
    }
}

// MARK: - Guided automatic remeshing

extension Mesh {
    /// Runs the automatic quad remesher with user guidance: strokes that bias
    /// the cross field, a painted density field, or both.
    ///
    /// This is the third of the three ways a drawn gesture becomes quads, and
    /// the only one that is not manual construction — a guide does not place
    /// geometry, it tells the solver which way the edge loops should run near
    /// the stroke, and the solver still decides the topology.
    ///
    /// With empty guidance the result is byte-for-byte identical to an
    /// unguided remesh, which the engine guarantees structurally rather than by
    /// tuning: every guidance code path sits behind an empty check.
    ///
    /// `onWarning` receives one message per clamp, per rejected guide and per
    /// island whose backend could not honour the guidance — guidance is never
    /// silently dropped, so ignoring this closure means ignoring a real answer.
    public func remeshGuided(
        params: RemeshParameters,
        guidance: Guidance,
        onWarning: ((String) -> Void)? = nil
    ) throws -> Mesh {
        var warnings: [String] = []
        var out: OpaquePointer?
        var cparams = params.cValue
        let status = ZRemesherOperation.withGuidance(guidance) { guidancePtr in
            withUnsafeMutablePointer(to: &warnings) { box in
                cyber_remesh_guided_ex(
                    handle, &cparams, guidancePtr, nil, nil,
                    { message, user in
                        guard let message, let user else { return }
                        user.assumingMemoryBound(to: [String].self).pointee.append(
                            String(cString: message))
                    }, UnsafeMutableRawPointer(box), &out)
            }
        }
        for message in warnings { onWarning?(message) }
        try CyberError.check(status)
        guard let out else { throw CyberError.outOfMemory }
        return Mesh(owning: out)
    }
}
