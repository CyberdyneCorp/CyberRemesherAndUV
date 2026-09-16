// Element queries and picking on `Mesh`, plus the remaining whole-mesh verbs.
//
// These are what an interactive host calls between gestures: what is under the
// finger, which loop does this edge belong to, how big is the mesh now. They
// were reachable from C and Python and not from Swift, which meant a mobile
// host could build geometry and then could not ask anything about it.

import CCyberRemesher

extension Mesh {

    // MARK: - Counts and elements

    /// Live edges.
    public var edgeCount: Int { cyber_mesh_edge_count(handle) }

    /// Live faces, as face ids.
    public var liveFaces: [UInt32] {
        let needed = cyber_mesh_live_faces(handle, nil, 0)
        guard needed > 0 else { return [] }
        var out = [UInt32](repeating: 0, count: needed)
        let written = out.withUnsafeMutableBufferPointer {
            cyber_mesh_live_faces(handle, $0.baseAddress, needed)
        }
        return Array(out.prefix(written))
    }

    /// The two vertices of `edge`, or `nil` when it is dead.
    public func edgeEndpoints(_ edge: UInt32) -> (a: UInt32, b: UInt32)? {
        var pair: (UInt32, UInt32) = (0, 0)
        let ok = withUnsafeMutablePointer(to: &pair) { p in
            p.withMemoryRebound(to: UInt32.self, capacity: 2) {
                cyber_mesh_edge_endpoints(handle, edge, $0)
            }
        }
        guard ok != 0 else { return nil }
        return (pair.0, pair.1)
    }

    /// Faces incident to `edge`, in deterministic radial order, with each
    /// face's side count — enough to tell whether a picked boundary edge
    /// borders a quad or a triangle.
    ///
    /// At most two are reported even on a non-manifold edge, which the engine
    /// supports; ``edgeFaceCount(_:)`` gives the true valence.
    public func edgeFaces(_ edge: UInt32) -> [(face: UInt32, sides: Int)] {
        var faces: (UInt32, UInt32) = (0, 0)
        var sides: (Int, Int) = (0, 0)
        let written = withUnsafeMutablePointer(to: &faces) { f in
            withUnsafeMutablePointer(to: &sides) { z in
                f.withMemoryRebound(to: UInt32.self, capacity: 2) { fp in
                    z.withMemoryRebound(to: Int.self, capacity: 2) { zp in
                        cyber_mesh_edge_faces(handle, edge, fp, zp)
                    }
                }
            }
        }
        guard written > 0 else { return [] }
        let all = [(faces.0, sides.0), (faces.1, sides.1)]
        return Array(all.prefix(Int(written)))
    }

    /// True number of live faces on `edge`, unclamped: 0 wire, 1 boundary,
    /// 2 manifold interior, 3+ non-manifold.
    public func edgeFaceCount(_ edge: UInt32) -> Int {
        Int(max(cyber_mesh_edge_face_count(handle, edge), 0))
    }

    /// Whether `edge` has exactly one incident face.
    public func isBoundaryEdge(_ edge: UInt32) -> Bool {
        cyber_mesh_is_boundary_edge(handle, edge) != 0
    }

    /// Position of one vertex, or `nil` when it is dead.
    public func vertexPosition(_ vertex: UInt32) -> SIMD3<Float>? {
        var xyz = (Float(0), Float(0), Float(0))
        let ok = withUnsafeMutablePointer(to: &xyz) { p in
            p.withMemoryRebound(to: Float.self, capacity: 3) {
                cyber_mesh_vertex_position(handle, vertex, $0)
            }
        }
        guard ok != 0 else { return nil }
        return SIMD3(xyz.0, xyz.1, xyz.2)
    }

    /// Counter that changes whenever an edit may have reassigned element ids.
    ///
    /// This is how a host knows its stored ids — a pin list, a loop tag, a
    /// mapping back into its own scene — still mean what they meant. Equal
    /// values prove the ids are good; differing values only mean do not assume.
    public var topologyGeneration: UInt64 { cyber_mesh_topology_generation(handle) }

    // MARK: - Picking

    /// Nearest live vertex to `query` within `maxDistance`.
    public func nearestVertex(to query: SIMD3<Float>, maxDistance: Float) -> UInt32? {
        var out: UInt32 = 0
        let found = withUnsafeXYZ(query) {
            cyber_mesh_nearest_vertex(handle, $0, maxDistance, &out, nil)
        }
        return found != 0 ? out : nil
    }

    /// Nearest live vertex to `query`, ignoring `excluding` — the "weld onto
    /// something that is not myself" query a drag needs.
    public func nearestVertex(to query: SIMD3<Float>, maxDistance: Float, excluding: UInt32)
        -> UInt32?
    {
        var out: UInt32 = 0
        let found = withUnsafeXYZ(query) {
            cyber_mesh_nearest_vertex_excluding(handle, $0, maxDistance, excluding, &out, nil)
        }
        return found != 0 ? out : nil
    }

    /// Nearest live edge to `query` within `maxDistance`.
    public func nearestEdge(to query: SIMD3<Float>, maxDistance: Float) -> UInt32? {
        var out: UInt32 = 0
        let found = withUnsafeXYZ(query) {
            cyber_mesh_nearest_edge(handle, $0, maxDistance, &out, nil)
        }
        return found != 0 ? out : nil
    }

    /// The quad ring through `edge` — the perpendicular partner to
    /// ``edgeLoop(through:)`` — and whether it wraps.
    public func quadRing(through edge: UInt32) -> (edges: [UInt32], isClosed: Bool) {
        var closed: Int32 = 0
        let needed = cyber_mesh_quad_ring(handle, edge, nil, 0, &closed)
        guard needed > 0 else { return ([], false) }
        var out = [UInt32](repeating: 0, count: needed)
        let written = out.withUnsafeMutableBufferPointer {
            cyber_mesh_quad_ring(handle, edge, $0.baseAddress, needed, &closed)
        }
        return (Array(out.prefix(written)), closed != 0)
    }

    /// Shortest vertex path between two vertices, as ordered vertex ids —
    /// what turns two taps into a seam or a cut line.
    public func shortestPath(from a: UInt32, to b: UInt32) -> [UInt32] {
        let needed = cyber_mesh_shortest_vertex_path(handle, a, b, nil, 0)
        guard needed > 0 else { return [] }
        var out = [UInt32](repeating: 0, count: needed)
        let written = out.withUnsafeMutableBufferPointer {
            cyber_mesh_shortest_vertex_path(handle, a, b, $0.baseAddress, needed)
        }
        return Array(out.prefix(written))
    }

    // MARK: - Visibility and tagging

    /// Hides faces from the viewport without deleting them — lasso hide.
    /// Passing an empty array shows everything again.
    public func setHiddenFaces(_ faces: [UInt32]) throws {
        try CyberError.check(
            faces.withUnsafeBufferPointer {
                cyber_mesh_set_hidden_faces(handle, $0.baseAddress, faces.count)
            })
    }

    /// How many faces are currently hidden.
    public var hiddenFaceCount: Int { cyber_mesh_hidden_face_count(handle) }

    /// Marks edges with a persistent tag — landmark loop colouring, and the
    /// same channel UV seams travel on.
    public func setTaggedEdges(_ edges: [UInt32]) throws {
        try CyberError.check(
            edges.withUnsafeBufferPointer {
                cyber_mesh_set_tagged_edges(handle, $0.baseAddress, edges.count)
            })
    }

    // MARK: - Whole-mesh verbs

    /// Triangulates every face, returning the resulting face count.
    @discardableResult
    public func triangulate() throws -> Int {
        var faces = 0
        try CyberError.check(cyber_retopo_triangulate(handle, &faces))
        return faces
    }

    /// Subdivides every face; `smooth` selects Catmull-Clark over linear.
    /// Both produce the same topology and differ only in where vertices land.
    @discardableResult
    public func subdivide(smooth: Bool = false, snapper: Snapper? = nil) throws -> Int {
        var faces = 0
        try CyberError.check(
            cyber_retopo_subdivide_ex(
                handle, snapper?.handle,
                smooth ? CYBER_SUBDIV_CATMULL_CLARK : CYBER_SUBDIV_LINEAR, &faces))
        return faces
    }

    /// Removes faces whose centroid lies within a pressure-scaled radius —
    /// the erase brush. Returns how many were removed.
    @discardableResult
    public func erase(center: SIMD3<Float>, baseRadius: Float, pressure: Float = 1) throws -> Int {
        var removed = 0
        try CyberError.check(
            withUnsafeXYZ(center) {
                cyber_retopo_erase(handle, $0, baseRadius, pressure, &removed)
            })
        return removed
    }

    /// Moves the vertices around `seedVertex` by `displacement`, falling off
    /// over `radius` and re-projecting onto the Target when a snapper is given.
    ///
    /// The brush is seeded from a VERTEX rather than a point in space, so the
    /// falloff runs over mesh connectivity — a drag on one side of a thin limb
    /// does not pull the other side with it.
    public func move(
        seedVertex: UInt32, displacement: SIMD3<Float>, radius: Float,
        pinned: [UInt32] = [], snapper: Snapper? = nil
    ) throws {
        try CyberError.check(
            withUnsafeXYZ(displacement) { d in
                pinned.withUnsafeBufferPointer { p in
                    cyber_retopo_move(
                        handle, seedVertex, d, radius, p.baseAddress, pinned.count,
                        snapper?.handle)
                }
            })
    }

    /// Redistributes the vertices along the path between two endpoints so they
    /// are evenly spaced.
    public func distributePath(_ vertices: [UInt32], snapper: Snapper? = nil) throws {
        try CyberError.check(
            vertices.withUnsafeBufferPointer {
                cyber_retopo_distribute_path(
                    handle, $0.baseAddress, vertices.count, snapper?.handle)
            })
    }

    /// Applies an affine to a vertex list, re-snapping onto the Target.
    @discardableResult
    public func transformVertices(
        _ vertices: [UInt32], transform: [Float], snapper: Snapper? = nil,
        resnapEpsilon: Float = 0
    ) throws -> (resnapped: Int, maxDistance: Float) {
        guard transform.count == 12 else {
            throw CyberError.invalidArgument("transformVertices needs a 12-float affine")
        }
        var resnapped = 0
        var maxDistance: Float = 0
        try CyberError.check(
            vertices.withUnsafeBufferPointer { v in
                transform.withUnsafeBufferPointer { xf in
                    cyber_retopo_transform_vertices(
                        handle, v.baseAddress, vertices.count, xf.baseAddress, snapper?.handle,
                        resnapEpsilon, &resnapped, &maxDistance)
                }
            })
        return (resnapped, maxDistance)
    }

    /// Isotropic triangle remeshing — the one remesher that also decimates.
    public func isotropicRemesh(targetEdgeLength: Float, iterations: Int = 3) throws {
        var params = CyberIsotropicParams()
        cyber_default_isotropic_params(&params)
        params.targetEdgeLength = targetEdgeLength
        params.iterations = Int32(iterations)
        try CyberError.check(cyber_mesh_isotropic_remesh(handle, &params, nil))
    }
}
