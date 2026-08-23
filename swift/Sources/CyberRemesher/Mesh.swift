// RAII wrapper over the opaque `CyberMesh` handle. Owns the handle for its
// lifetime and releases it in `deinit` with `cyber_mesh_destroy`, so Swift ARC
// drives engine memory. The class is `final` and not `Sendable`: a mesh handle
// is single-owner and must not be mutated from two tasks at once (remesh
// produces a *new* Mesh instead).
//
// The C ABI has no bulk "set topology" entry point, so `init(positions:indices:)`
// builds the mesh one face at a time through `cyber_retopo_build_face` — the
// documented weld primitive — mapping each source index to the engine vertex id
// the first face that used it created. That is O(triangles) ABI calls: fine for
// procedural and test geometry, but load real assets with `loadOBJ(_:)`.

import CCyberRemesher

/// A triangle or quad-dominant mesh owned by the engine.
///
/// Construct from indexed geometry with ``init(positions:indices:)``, or read a
/// Wavefront OBJ with ``loadOBJ(_:)``; read it back with ``positions()`` /
/// ``triangleIndices()``; remesh it with ``remesh(params:)`` (see `Remesh.swift`).
public final class Mesh {
    /// The opaque engine handle. Non-optional for a live instance.
    let handle: OpaquePointer

    /// Wraps an already-owned engine handle. Takes ownership.
    init(owning handle: OpaquePointer) {
        self.handle = handle
    }

    /// An empty mesh.
    public convenience init() throws {
        guard let handle = cyber_mesh_create() else { throw CyberError.outOfMemory }
        self.init(owning: handle)
    }

    /// Builds a mesh from a flat position buffer and triangle indices.
    ///
    /// - Parameters:
    ///   - positions: `x,y,z` triples; `count` must be a multiple of 3.
    ///   - indices: triangle corner indices; `count` a multiple of 3.
    /// - Throws: ``CyberError`` on malformed buffers, an out-of-range index, or
    ///   a degenerate triangle (the engine rejects repeated or coincident ring
    ///   corners rather than creating a zero-area face).
    public convenience init(positions: [Float], indices: [UInt32]) throws {
        guard positions.count % 3 == 0 else {
            throw CyberError.invalidArgument("positions count must be a multiple of 3")
        }
        guard indices.count % 3 == 0 else {
            throw CyberError.invalidArgument("indices count must be a multiple of 3")
        }
        guard let handle = cyber_mesh_create() else { throw CyberError.outOfMemory }
        do {
            try Mesh.buildTriangles(into: handle, positions: positions, indices: indices)
        } catch {
            cyber_mesh_destroy(handle)
            throw error
        }
        self.init(owning: handle)
    }

    /// Appends every triangle of an indexed buffer to a fresh engine handle.
    ///
    /// `engineIds` starts as `CYBER_BUILD_NEW_VERTEX` for every source vertex,
    /// which tells `cyber_retopo_build_face` to create it; the ids it reports
    /// back are reused by later faces, which is what welds the mesh together.
    private static func buildTriangles(
        into handle: OpaquePointer, positions: [Float], indices: [UInt32]
    ) throws {
        let vertexCount = positions.count / 3
        var engineIds = [UInt32](repeating: CYBER_BUILD_NEW_VERTEX, count: vertexCount)
        var ring = [UInt32](repeating: 0, count: 3)
        var corners = [Float](repeating: 0, count: 9)
        var created = [UInt32](repeating: 0, count: 3)

        for triangle in stride(from: 0, to: indices.count, by: 3) {
            for corner in 0..<3 {
                let source = Int(indices[triangle + corner])
                guard source < vertexCount else {
                    throw CyberError.invalidArgument(
                        "index \(source) is out of range for \(vertexCount) vertices")
                }
                ring[corner] = engineIds[source]
                corners[corner * 3 + 0] = positions[source * 3 + 0]
                corners[corner * 3 + 1] = positions[source * 3 + 1]
                corners[corner * 3 + 2] = positions[source * 3 + 2]
            }
            try CyberError.check(
                cyber_retopo_build_face(handle, 3, &ring, &corners, nil, nil, &created))
            for corner in 0..<3 {
                engineIds[Int(indices[triangle + corner])] = created[corner]
            }
        }
    }

    deinit {
        cyber_mesh_destroy(handle)
    }

    /// Number of live vertices.
    public var vertexCount: Int {
        Int(cyber_mesh_vertex_count(handle))
    }

    /// Number of live faces (triangles or quads depending on engine state).
    public var faceCount: Int {
        Int(cyber_mesh_face_count(handle))
    }

    /// Number of triangles the live faces fan-triangulate to (an n-gon counts
    /// `n - 2`, so a pure-quad mesh reports `2 * faceCount`).
    public var triangleCount: Int {
        Int(cyber_mesh_triangle_count(handle))
    }

    /// Copies vertex positions out as a flat `x,y,z` buffer, in the engine's
    /// compacted order (live vertices in id order).
    ///
    /// This is the stream ``setPositions(_:)`` writes back, so it always covers
    /// EVERY live vertex. The C ABI has a second, narrower "render vertex
    /// order" that drops vertices used only by hidden faces; nothing in this
    /// package can hide a face, so for a Swift-owned mesh the two orders are
    /// the same and the buffers below pair with this one.
    public func positions() -> [Float] {
        copyFloats(cyber_mesh_copy_positions)
    }

    /// Copies per-vertex unit normals out, in the render vertex order — the
    /// same order as ``positions()`` for a mesh built through this package.
    public func normals() -> [Float] {
        copyFloats(cyber_mesh_copy_normals)
    }

    /// Copies the fan-triangulated index buffer out (3 per triangle), indexing
    /// the render vertex order — the same order as ``positions()`` for a mesh
    /// built through this package.
    public func triangleIndices() -> [UInt32] {
        copyIndices(cyber_mesh_copy_triangle_indices)
    }

    /// Copies the unique authored-edge index buffer out (2 per edge), indexing
    /// the same order as ``triangleIndices()``. A quad contributes its 4 edges,
    /// never its triangulation diagonal.
    public func edgeIndices() -> [UInt32] {
        copyIndices(cyber_mesh_copy_edge_indices)
    }

    /// Writes vertex positions back — the exact inverse of ``positions()``.
    ///
    /// - Throws: ``CyberError/invalidArgument(_:)`` when the count is not
    ///   exactly `3 * vertexCount` (a partial write would pair positions with
    ///   the wrong vertices, so the ABI rejects it and leaves the mesh alone).
    public func setPositions(_ positions: [Float]) throws {
        try CyberError.check(
            positions.withUnsafeBufferPointer {
                cyber_mesh_set_positions(handle, $0.baseAddress, $0.count)
            })
    }

    /// Deep-copies into a fresh independent handle. Element ids are preserved,
    /// so any id-keyed caller state stays valid against the copy — which is
    /// what makes this usable as an undo snapshot.
    public func clone() throws -> Mesh {
        var out: OpaquePointer?
        try CyberError.check(cyber_mesh_clone(handle, &out))
        guard let copy = out else { throw CyberError.outOfMemory }
        return Mesh(owning: copy)
    }

    /// Topology summary of the mesh.
    public func stats() throws -> MeshStats {
        var raw = CyberStats()
        try CyberError.check(cyber_mesh_stats(handle, &raw))
        return MeshStats(
            vertices: Int(raw.vertices),
            quads: Int(raw.quads),
            triangles: Int(raw.triangles),
            other: Int(raw.other),
            islands: Int(raw.islands),
            islandsFailed: Int(raw.islandsFailed)
        )
    }

    /// Loads a mesh from a Wavefront OBJ.
    public static func loadOBJ(_ path: String) throws -> Mesh {
        var out: OpaquePointer?
        try CyberError.check(path.withCString { cyber_mesh_load_obj($0, &out) })
        guard let handle = out else {
            throw CyberError.io("mesh load returned a null handle for \(path)")
        }
        return Mesh(owning: handle)
    }

    /// Writes the mesh to a Wavefront OBJ (a sibling `.mtl` may be written too).
    public func saveOBJ(to path: String) throws {
        try CyberError.check(path.withCString { cyber_mesh_save_obj(handle, $0) })
    }

    // MARK: - Private

    /// `copy_positions` convention: query the required count, then fill.
    private func copyFloats(
        _ reader: (OpaquePointer?, UnsafeMutablePointer<Float>?, Int) -> Int
    ) -> [Float] {
        let count = reader(handle, nil, 0)
        guard count > 0 else { return [] }
        return [Float](unsafeUninitializedCapacity: count) { buffer, initialized in
            initialized = reader(handle, buffer.baseAddress, buffer.count)
        }
    }

    private func copyIndices(
        _ reader: (OpaquePointer?, UnsafeMutablePointer<UInt32>?, Int) -> Int
    ) -> [UInt32] {
        let count = reader(handle, nil, 0)
        guard count > 0 else { return [] }
        return [UInt32](unsafeUninitializedCapacity: count) { buffer, initialized in
            initialized = reader(handle, buffer.baseAddress, buffer.count)
        }
    }
}

/// Topology summary produced by ``Mesh/stats()``.
public struct MeshStats: Equatable, Sendable {
    public var vertices: Int
    public var quads: Int
    public var triangles: Int
    /// Faces that are neither triangles nor quads.
    public var other: Int
    /// Connected components.
    public var islands: Int
    /// Islands the pipeline could not remesh (0 for meshes not produced by a
    /// remesh).
    public var islandsFailed: Int
}
