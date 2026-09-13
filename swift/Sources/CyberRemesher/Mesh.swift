// RAII wrapper over the opaque `CyberMesh` handle. Owns the handle for its
// lifetime and releases it in `deinit` with `cyber_mesh_destroy`, so Swift ARC
// drives engine memory. The class is `final` and not `Sendable`: a mesh handle
// is single-owner and must not be mutated from two tasks at once (remesh
// produces a *new* Mesh instead).
//
// `init(positions:faceOffsets:indices:attributes:)` copies indexed geometry in
// one C ABI call. The triangle convenience initializer forwards to that bulk
// path, so procedural and production geometry share the same ownership and
// validation contract.

import CCyberRemesher

public enum MeshAttributeDomain: Int32 { case vertex, face, corner }

public enum MeshAttributeValues {
    case float([Float]), int32([Int32]), float2([Float]), float3([Float]), float4([Float])

    fileprivate var type: Int32 {
        switch self { case .float: 0; case .int32: 1; case .float2: 2; case .float3: 3; case .float4: 4 }
    }
    fileprivate var width: Int { switch self { case .float, .int32: 1; case .float2: 2; case .float3: 3; case .float4: 4 } }
    fileprivate var scalarCount: Int {
        switch self { case let .float(v), let .float2(v), let .float3(v), let .float4(v): return v.count
        case let .int32(v): return v.count }
    }
    fileprivate func withUnsafeValues<R>(_ body: (UnsafeRawPointer?, Int) -> R) -> R {
        switch self {
        case let .float(values), let .float2(values), let .float3(values), let .float4(values):
            return values.withUnsafeBufferPointer { body(UnsafeRawPointer($0.baseAddress), values.count / width) }
        case let .int32(values):
            return values.withUnsafeBufferPointer { body(UnsafeRawPointer($0.baseAddress), values.count) }
        }
    }
}

/// A typed, copying attribute column. Corner values follow CSR polygon indices,
/// preserving UV seams; face values follow authored polygon order.
public struct MeshAttribute {
    public let name: String
    public let domain: MeshAttributeDomain
    public let values: MeshAttributeValues
    public init(name: String, domain: MeshAttributeDomain, values: MeshAttributeValues) {
        self.name = name; self.domain = domain; self.values = values
    }
}

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
        guard indices.count % 3 == 0 else {
            throw CyberError.invalidArgument("indices count must be a multiple of 3")
        }
        let offsets = stride(from: 0, through: indices.count, by: 3).map { $0 }
        try self.init(positions: positions, faceOffsets: offsets, indices: indices)
    }

    /// Builds a mesh from packed positions and CSR-authored polygons in one C
    /// ABI call. `faceOffsets` starts at zero and has one trailing entry; each
    /// range selects at least three entries from `indices`.
    ///
    /// The engine copies valid buffers before this initializer returns, so the
    /// supplied arrays may be immediately reused or mutated. Triangles, quads,
    /// and n-gons retain their authored arity; use ``authoredPolygons()`` to
    /// read that topology back rather than the render triangulation.
    public convenience init(
        positions: [Float], faceOffsets: [Int], indices: [UInt32], attributes: [MeshAttribute] = []
    ) throws {
        guard positions.count % 3 == 0 else {
            throw CyberError.invalidArgument("positions count must be a multiple of 3")
        }
        guard !faceOffsets.isEmpty, faceOffsets[0] == 0, faceOffsets.last == indices.count else {
            throw CyberError.invalidArgument("face offsets must span exactly the index buffer")
        }
        guard zip(faceOffsets, faceOffsets.dropFirst()).allSatisfy({ pair in
            pair.0 >= 0 && pair.1 >= pair.0 && pair.1 - pair.0 >= 3
        }) else {
            throw CyberError.invalidArgument("each face must have at least three corners")
        }
        let expected = [positions.count / 3, faceOffsets.count - 1, indices.count]
        for attribute in attributes {
            guard !attribute.name.utf8.isEmpty, !attribute.name.utf8.contains(0),
                  attribute.name.lengthOfBytes(using: .utf8) <= 63,
                  attribute.values.scalarCount % attribute.values.width == 0,
                  attribute.values.scalarCount / attribute.values.width == expected[Int(attribute.domain.rawValue)] else {
                throw CyberError.invalidArgument("attribute name, arity, or domain count is invalid")
            }
        }
        var handle: OpaquePointer?
        let status = Self.withAttributeColumns(attributes) { attributeColumns in positions.withUnsafeBufferPointer { positionsBuffer in
            faceOffsets.withUnsafeBufferPointer { offsetsBuffer in
                indices.withUnsafeBufferPointer { indicesBuffer in
                    var input = CyberIndexedMesh(
                        positions: positionsBuffer.baseAddress,
                        vertex_count: positions.count / 3,
                        face_offsets: offsetsBuffer.baseAddress,
                        face_count: faceOffsets.count - 1,
                        indices: indicesBuffer.baseAddress,
                        index_count: indices.count,
                        attributes: attributeColumns.baseAddress,
                        attribute_count: attributeColumns.count
                    )
                    return cyber_mesh_from_indexed(&input, &handle)
                }
            }
        } }
        try CyberError.check(status)
        guard let handle else { throw CyberError.outOfMemory }
        self.init(owning: handle)
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

    /// Returns CSR offsets and indices for authored triangles, quads, and
    /// n-gons. Unlike ``triangleIndices()``, this does not fan-triangulate.
    public func authoredPolygons() -> (faceOffsets: [Int], indices: [UInt32]) {
        let offsetCount = cyber_mesh_copy_face_offsets(handle, nil, 0)
        let indexCount = cyber_mesh_copy_polygon_indices(handle, nil, 0)
        var offsets = [Int](repeating: 0, count: offsetCount)
        var indices = [UInt32](repeating: 0, count: indexCount)
        _ = offsets.withUnsafeMutableBufferPointer {
            cyber_mesh_copy_face_offsets(handle, $0.baseAddress, $0.count)
        }
        _ = indices.withUnsafeMutableBufferPointer {
            cyber_mesh_copy_polygon_indices(handle, $0.baseAddress, $0.count)
        }
        return (offsets, indices)
    }

    /// Copies the complete authored attribute schema and values.
    public func authoredAttributes() throws -> [MeshAttribute] {
        var result: [MeshAttribute] = []
        for index in 0..<cyber_mesh_attribute_count(handle) {
            var info = CyberAttributeInfo()
            try CyberError.check(cyber_mesh_attribute_info(handle, index, &info))
            let name = withUnsafePointer(to: &info.name) {
                $0.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
            }
            let scalars = Int(cyber_mesh_copy_attribute(handle, &info, nil, 0))
            let values: MeshAttributeValues
            if info.type == 1 {
                var data = [Int32](repeating: 0, count: scalars)
                _ = data.withUnsafeMutableBufferPointer { cyber_mesh_copy_attribute(handle, &info, $0.baseAddress, $0.count) }
                values = .int32(data)
            } else {
                var data = [Float](repeating: 0, count: scalars)
                _ = data.withUnsafeMutableBufferPointer { cyber_mesh_copy_attribute(handle, &info, $0.baseAddress, $0.count) }
                switch info.type { case 0: values = .float(data); case 2: values = .float2(data); case 3: values = .float3(data); default: values = .float4(data) }
            }
            guard let domain = MeshAttributeDomain(rawValue: info.domain) else { continue }
            result.append(MeshAttribute(name: name, domain: domain, values: values))
        }
        return result
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

    private static func withAttributeColumns<R>(_ attributes: [MeshAttribute],
                                                _ body: (UnsafeBufferPointer<CyberAttributeColumn>) -> R) -> R {
        func walk(_ index: Int, _ columns: [CyberAttributeColumn]) -> R {
            guard index < attributes.count else { return columns.withUnsafeBufferPointer(body) }
            let attribute = attributes[index]
            return attribute.name.withCString { name in
                attribute.values.withUnsafeValues { values, count in
                    var next = columns
                    next.append(CyberAttributeColumn(name: name, domain: attribute.domain.rawValue,
                                                     type: attribute.values.type, values: values,
                                                     value_count: count))
                    return walk(index + 1, next)
                }
            }
        }
        return walk(0, [])
    }

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
