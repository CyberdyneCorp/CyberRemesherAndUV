// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the ABI contract in README.md.
//
// The document/session layer: an interactive editing context over a mesh. The
// ABI's session owns the tool command stack, undo history and the live edit
// mesh. The first-party iPadOS shell drives it exclusively through this type
// (no private hooks), which is what guarantees third parties get parity.

import CCyberRemesher

/// One editing session (document) over a mesh.
///
/// Feed it synthetic or real input via ``inject(stroke:)`` / ``inject(tapAt:)``
/// (see `InputForwarding.swift` for UIKit/PencilKit bridging) and read the live
/// result with ``snapshot()``.
public final class Session {
    let handle: OpaquePointer

    init(owning handle: OpaquePointer) {
        self.handle = handle
    }

    /// Opens a session over `mesh`. The engine copies what it needs; the caller
    /// may release `mesh` afterwards.
    public init(mesh: Mesh) throws {
        var out: OpaquePointer?
        try CyberError.check(cyber_session_create(mesh.handle, &out))
        guard let handle = out else {
            throw CyberError.outOfMemory
        }
        self.handle = handle
    }

    deinit {
        cyber_session_destroy(handle)
    }

    /// Returns the current edit mesh as an owned ``Mesh``.
    public func snapshot() throws -> Mesh {
        var out: OpaquePointer?
        try CyberError.check(cyber_session_snapshot_mesh(handle, &out))
        guard let mesh = out else {
            throw CyberError.pipeline("session produced a null snapshot")
        }
        return Mesh(owning: mesh)
    }

    /// Injects a completed stroke (relaxation, cut, tool paint…) into the
    /// session's stroke-grammar recognizer.
    public func inject(stroke samples: [StrokeSample]) throws {
        guard !samples.isEmpty else { return }
        let raw = samples.map(\.abi)
        let status = raw.withUnsafeBufferPointer {
            cyber_session_inject_stroke(handle, $0.baseAddress, $0.count)
        }
        try CyberError.check(status)
    }

    /// Injects a single tap at a normalized viewport coordinate.
    public func inject(tapAt point: StrokePoint) throws {
        try CyberError.check(cyber_session_inject_tap(handle, point.x, point.y))
    }

    /// Injects a chord (multi-touch/modifier command), identified by opaque
    /// button codes the ABI documents.
    public func inject(chord buttons: [UInt64]) throws {
        guard !buttons.isEmpty else { return }
        let status = buttons.withUnsafeBufferPointer {
            cyber_session_inject_chord(handle, $0.baseAddress, $0.count)
        }
        try CyberError.check(status)
    }
}

/// A normalized 2D point in viewport space (`0...1`, origin top-left).
public struct StrokePoint: Sendable {
    public var x: Double
    public var y: Double
    public init(x: Double, y: Double) {
        self.x = x
        self.y = y
    }
}

/// One sample along an input stroke. Carries Pencil-grade data (pressure and
/// tilt) so palm rejection and pressure-driven tools behave identically whether
/// the input came from a real UITouch or a recorded synthetic trace.
public struct StrokeSample: Sendable {
    public var location: StrokePoint
    /// Normalized pressure `0...1` (1.0 for non-pressure devices).
    public var pressure: Double
    /// Pencil altitude angle in radians (`.pi/2` for a perpendicular stylus).
    public var altitudeAngle: Double
    /// Pencil azimuth angle in radians.
    public var azimuthAngle: Double
    /// Monotonic timestamp in seconds since an arbitrary epoch.
    public var timestamp: Double

    public init(
        location: StrokePoint,
        pressure: Double = 1.0,
        altitudeAngle: Double = .pi / 2,
        azimuthAngle: Double = 0.0,
        timestamp: Double = 0.0
    ) {
        self.location = location
        self.pressure = pressure
        self.altitudeAngle = altitudeAngle
        self.azimuthAngle = azimuthAngle
        self.timestamp = timestamp
    }

    /// Lowers to the plain-C ABI struct.
    var abi: CyberStrokeSample {
        CyberStrokeSample(
            x: location.x,
            y: location.y,
            pressure: pressure,
            altitude_angle: altitudeAngle,
            azimuth_angle: azimuthAngle,
            timestamp: timestamp
        )
    }
}
