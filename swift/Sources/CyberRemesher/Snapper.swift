// RAII wrapper over the opaque `CyberSnapper` handle — the Target-surface
// hierarchy every interactive retopology tool projects onto.
//
// This is the binding the rest of the Swift surface was already written
// against and could not construct: `SoftSelection`'s `relaxSelection` and
// `transformSelection` have taken a `snapper: OpaquePointer?` since they were
// written, and no Swift caller had any way to produce one. A mobile host could
// classify a stroke with the full gesture grammar, and then had nothing to snap
// the resulting geometry to.
//
// A Snapper is a SNAPSHOT of the Target: build one after the Target changes.
// It is `final` and not `Sendable` for the same reason `Mesh` is — the handle
// is single-owner. It borrows nothing from the Mesh it was built from, so the
// Target may be released while the Snapper lives.

import CCyberRemesher

/// A point found on the Target surface.
public struct SurfaceHit: Equatable {
    public let point: SIMD3<Float>
    /// Face of the Target that owns the hit.
    public let face: UInt32
}

/// A Target vertex found near a query point.
public struct VertexHit: Equatable {
    public let point: SIMD3<Float>
    public let vertex: UInt32
}

/// A ray hit on the Target surface.
public struct RayHit: Equatable {
    public let point: SIMD3<Float>
    /// Distance along the ray, in the units the direction was given in.
    public let distance: Float
    public let face: UInt32
}

/// Accelerated closest-point, nearest-vertex and raycast queries against a
/// Target mesh.
///
/// Build one per Target and keep it for the life of that Target:
/// ```swift
/// let snapper = try Snapper(target: sculpt)
/// if let hit = snapper.raycast(origin: eye, direction: ray) {
///     // hit.point is on the sculpt's surface
/// }
/// ```
public final class Snapper {
    let handle: OpaquePointer

    /// Builds the hierarchy over `target`. The Target is only read; the
    /// snapper holds no reference to it afterwards.
    public init(target: Mesh) throws {
        var raw: OpaquePointer?
        try CyberError.check(cyber_snapper_create(target.handle, &raw))
        guard let raw else { throw CyberError.outOfMemory }
        handle = raw
    }

    deinit { cyber_snapper_free(handle) }

    /// Closest point on the Target surface to `query`, or `nil` when the
    /// snapper is empty.
    public func snapToSurface(_ query: SIMD3<Float>) -> SurfaceHit? {
        var point = (Float(0), Float(0), Float(0))
        var face: UInt32 = 0
        let found = withUnsafeXYZ(query) { q in
            withUnsafeMutablePointer(to: &point) { p in
                p.withMemoryRebound(to: Float.self, capacity: 3) { out in
                    cyber_snapper_snap_to_surface(handle, q, out, &face)
                }
            }
        }
        guard found != 0 else { return nil }
        return SurfaceHit(point: SIMD3(point.0, point.1, point.2), face: face)
    }

    /// Nearest Target *vertex* within `radius` — the snap-to-vertex modifier,
    /// which is what makes a new vertex land exactly on an existing feature
    /// corner instead of near it.
    public func snapToVertex(_ query: SIMD3<Float>, radius: Float) -> VertexHit? {
        var point = (Float(0), Float(0), Float(0))
        var vertex: UInt32 = 0
        let found = withUnsafeXYZ(query) { q in
            withUnsafeMutablePointer(to: &point) { p in
                p.withMemoryRebound(to: Float.self, capacity: 3) { out in
                    cyber_snapper_snap_to_vertex(handle, q, radius, out, &vertex)
                }
            }
        }
        guard found != 0 else { return nil }
        return VertexHit(point: SIMD3(point.0, point.1, point.2), vertex: vertex)
    }

    /// First Target hit along `origin + t * direction` for `t` in
    /// `(0, maxDistance]`. `direction` need not be normalized.
    ///
    /// This is the viewport tap: unproject the touch, cast, and the returned
    /// point is where the artist actually pointed on the sculpt.
    public func raycast(
        origin: SIMD3<Float>,
        direction: SIMD3<Float>,
        maxDistance: Float = .greatestFiniteMagnitude
    ) -> RayHit? {
        var point = (Float(0), Float(0), Float(0))
        var t: Float = 0
        var face: UInt32 = 0
        let found = withUnsafeXYZ(origin) { o in
            withUnsafeXYZ(direction) { d in
                withUnsafeMutablePointer(to: &point) { p in
                    p.withMemoryRebound(to: Float.self, capacity: 3) { out in
                        cyber_snapper_raycast(handle, o, d, maxDistance, out, &t, &face)
                    }
                }
            }
        }
        guard found != 0 else { return nil }
        return RayHit(point: SIMD3(point.0, point.1, point.2), distance: t, face: face)
    }
}

/// Calls `body` with a 3-float buffer holding `v`.
///
/// `SIMD3<Float>` is 16 bytes with a padding lane, so handing its address
/// straight to a `const float[3]` parameter would be correct only by accident
/// of layout. Copying through a tuple keeps it exactly three floats.
@inline(__always)
func withUnsafeXYZ<R>(_ v: SIMD3<Float>, _ body: (UnsafePointer<Float>) -> R) -> R {
    var xyz = (v.x, v.y, v.z)
    return withUnsafePointer(to: &xyz) { p in
        p.withMemoryRebound(to: Float.self, capacity: 3) { body($0) }
    }
}

/// Flattens points to the x,y,z triplets the ABI takes.
@inline(__always)
func flatten(_ points: [SIMD3<Float>]) -> [Float] {
    var out = [Float]()
    out.reserveCapacity(points.count * 3)
    for p in points {
        out.append(p.x)
        out.append(p.y)
        out.append(p.z)
    }
    return out
}

// MARK: - Device resource ceilings

/// Process-wide resource ceilings.
///
/// Off by default, deliberately: the engine cannot know a host's budget, and a
/// value chosen inside the library would be too small for a workstation and
/// useless on a phone. A mobile host is exactly the caller that must set them —
/// an iPad that imports a 200 M-vertex scan should refuse it, not be killed by
/// the OS mid-parse.
///
/// Each ceiling bounds a different resource, so setting one says nothing about
/// the others. A value of 0 means unbounded.
extension CyberRuntime {
    /// Largest input file, in bytes, any importer will read.
    public static func setMaxImportInputBytes(_ bytes: UInt64) throws {
        try CyberError.check(cyber_set_max_import_input_bytes(bytes))
    }

    /// Largest vertex count an import may RETURN. This bounds the mesh, not the
    /// peak parse allocation — the useful boundary anyway, since loading is the
    /// cheap half and this refuses the mesh before remeshing spends orders of
    /// magnitude more on it.
    public static func setMaxImportVertices(_ count: UInt64) throws {
        try CyberError.check(cyber_set_max_import_vertices(count))
    }

    /// Largest face count an import may return.
    public static func setMaxImportFaces(_ count: UInt64) throws {
        try CyberError.check(cyber_set_max_import_faces(count))
    }

    /// Largest texel count a bake may allocate.
    public static func setMaxBakePixels(_ pixels: UInt64) throws {
        try CyberError.check(cyber_set_max_bake_pixels(pixels))
    }

    /// Caps the engine's worker threads — the difference between a responsive
    /// app and one whose UI starves while a solve runs.
    public static func setMaxWorkerThreads(_ threads: Int) throws {
        try CyberError.check(cyber_set_max_worker_threads(Int32(threads)))
    }

    /// Current ceilings, as set (0 means unbounded).
    public static var maxImportInputBytes: UInt64 { cyber_max_import_input_bytes() }
    public static var maxImportVertices: UInt64 { cyber_max_import_vertices() }
    public static var maxImportFaces: UInt64 { cyber_max_import_faces() }
    public static var maxBakePixels: UInt64 { cyber_max_bake_pixels() }
}
