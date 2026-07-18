// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the ABI contract in README.md.
//
// RAII wrapper over the opaque `CyberMesh` handle. Owns the handle for its
// lifetime and destroys it in `deinit`, so Swift ARC drives engine memory. The
// class is `final` and not `Sendable`: a mesh handle is single-owner and must
// not be mutated from two tasks at once (remesh produces a *new* Mesh instead).

import CCyberRemesher

/// A triangle or quad-dominant mesh owned by the engine.
///
/// Construct from indexed geometry with ``init(positions:indices:)``; read it
/// back with ``positions()`` / ``indices()``; remesh it with
/// ``remesh(params:)`` (see `Remesh.swift`).
public final class Mesh {
    /// The opaque engine handle. Non-optional for a live instance.
    let handle: OpaquePointer

    /// Wraps an already-owned engine handle. Takes ownership.
    init(owning handle: OpaquePointer) {
        self.handle = handle
    }

    /// Builds a mesh from a flat position buffer and triangle indices.
    ///
    /// - Parameters:
    ///   - positions: `x,y,z` triples; `count` must be a multiple of 3.
    ///   - indices: triangle corner indices; `count` a multiple of 3.
    /// - Throws: ``CyberError`` on invalid buffers or allocation failure.
    public convenience init(positions: [Float], indices: [UInt32]) throws {
        guard positions.count % 3 == 0 else {
            throw CyberError.invalidArgument("positions count must be a multiple of 3")
        }
        guard indices.count % 3 == 0 else {
            throw CyberError.invalidArgument("indices count must be a multiple of 3")
        }
        var out: OpaquePointer?
        let status = positions.withUnsafeBufferPointer { pos in
            indices.withUnsafeBufferPointer { idx in
                cyber_mesh_create_indexed(
                    pos.baseAddress,
                    pos.count / 3,
                    idx.baseAddress,
                    idx.count,
                    &out
                )
            }
        }
        try CyberError.check(status)
        guard let handle = out else {
            throw CyberError.outOfMemory
        }
        self.init(owning: handle)
    }

    deinit {
        cyber_mesh_destroy(handle)
    }

    /// Number of vertices.
    public var vertexCount: Int {
        Int(cyber_mesh_vertex_count(handle))
    }

    /// Number of faces (triangles or quads depending on engine state).
    public var faceCount: Int {
        Int(cyber_mesh_face_count(handle))
    }

    /// Copies vertex positions out as a flat `x,y,z` buffer.
    public func positions() -> [Float] {
        let count = Int(cyber_mesh_vertex_count(handle)) * 3
        guard count > 0 else { return [] }
        return [Float](unsafeUninitializedCapacity: count) { buffer, initialized in
            let written = cyber_mesh_copy_positions(handle, buffer.baseAddress, buffer.count)
            initialized = Int(written)
        }
    }

    /// Copies the face-corner index buffer out.
    public func indices() -> [UInt32] {
        let count = Int(cyber_mesh_index_count(handle))
        guard count > 0 else { return [] }
        return [UInt32](unsafeUninitializedCapacity: count) { buffer, initialized in
            let written = cyber_mesh_copy_indices(handle, buffer.baseAddress, buffer.count)
            initialized = Int(written)
        }
    }

    /// Loads a mesh from a file (OBJ/PLY/STL/glTF — engine-detected by extension).
    public static func load(contentsOf path: String) throws -> Mesh {
        var out: OpaquePointer?
        let status = path.withCString { cyber_mesh_load($0, &out) }
        try CyberError.check(status)
        guard let handle = out else {
            throw CyberError.io("mesh load returned a null handle for \(path)")
        }
        return Mesh(owning: handle)
    }

    /// Writes the mesh to a file (format inferred from the extension).
    public func save(to path: String) throws {
        let status = path.withCString { cyber_mesh_save(handle, $0) }
        try CyberError.check(status)
    }
}
