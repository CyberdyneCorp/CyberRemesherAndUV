// Async/await remeshing. The engine's `cyber_remesh` is a blocking C call that
// takes a POD `CyberRemeshParams` and C function-pointer callbacks for progress
// and cancellation, sharing ONE opaque `user` pointer between them. This file
// bridges it to Swift concurrency:
//   * progress -> an `AsyncStream<Double>` the caller can `for await` over;
//   * Swift `Task` cancellation -> the ABI's `CyberCancelCb`, which the engine
//     polls cooperatively and unwinds leaving inputs untouched.
// The blocking call runs on a dedicated thread so it never stalls the Swift
// cooperative thread pool.

import CCyberRemesher
import Foundation

/// Which quadrangulator `cyber_remesh` runs, mirroring the `CYBER_QUAD_*`
/// values of `CyberRemeshParams.quadMethod`.
public struct QuadMethod: RawRepresentable, Equatable, Sendable {
    public var rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }

    public static let fieldAligned = QuadMethod(rawValue: Int32(CYBER_QUAD_FIELD_ALIGNED))
    public static let instantMeshes = QuadMethod(rawValue: Int32(CYBER_QUAD_INSTANT_MESHES))
    public static let integer = QuadMethod(rawValue: Int32(CYBER_QUAD_INTEGER))
    /// QuadCover seamless-UV isoline extractor — the engine default. Falls back
    /// to field-aligned in builds with no seamless-UV solver.
    public static let quadCover = QuadMethod(rawValue: Int32(CYBER_QUAD_QUADCOVER))
}

/// User-facing remeshing parameters — a Swift mirror of `CyberRemeshParams`.
///
/// The memberwise defaults come from the engine itself (`cyber_default_params`),
/// so a freshly initialized value always matches the CLI's behaviour.
public struct RemeshParameters: Sendable {
    /// Desired output quad count.
    public var targetQuads: Int
    /// Multiplier on the derived edge length.
    public var edgeScale: Float
    /// Dihedral threshold (degrees) for feature edges.
    public var sharpEdgeDegrees: Float
    /// Normal-smoothing angle in degrees.
    public var smoothNormalDegrees: Float
    /// 0 uniform .. 1 fully curvature-adaptive.
    public var adaptivity: Float
    /// Forbid residual triangles in the output.
    public var pureQuads: Bool
    /// Maximum boundary-edge count of holes to fill; 0 disables hole filling
    /// and preserves open rims.
    public var holeFillMaxBoundary: Int
    /// Which quadrangulator to run.
    public var quadMethod: QuadMethod

    /// The engine defaults, read back through the ABI.
    public init() {
        var defaults = CyberRemeshParams()
        cyber_default_params(&defaults)
        targetQuads = Int(defaults.targetQuads)
        edgeScale = defaults.edgeScale
        sharpEdgeDegrees = defaults.sharpEdgeDegrees
        smoothNormalDegrees = defaults.smoothNormalDegrees
        adaptivity = defaults.adaptivity
        pureQuads = defaults.pureQuads != 0
        holeFillMaxBoundary = Int(defaults.holeFillMaxBoundary)
        quadMethod = QuadMethod(rawValue: defaults.quadMethod)
    }

    /// The engine defaults with a target quad count applied.
    public init(targetQuads: Int) {
        self.init()
        self.targetQuads = targetQuads
    }

    /// Lowers to the plain-C ABI struct.
    var cValue: CyberRemeshParams {
        CyberRemeshParams(
            targetQuads: Int32(clamping: targetQuads),
            edgeScale: edgeScale,
            sharpEdgeDegrees: sharpEdgeDegrees,
            smoothNormalDegrees: smoothNormalDegrees,
            adaptivity: adaptivity,
            pureQuads: pureQuads ? 1 : 0,
            holeFillMaxBoundary: Int32(clamping: holeFillMaxBoundary),
            quadMethod: quadMethod.rawValue
        )
    }
}

/// Shared control block handed to the C callbacks via the opaque `user` pointer.
///
/// Held strongly by the running thread closure for the whole blocking call, so
/// the `Unmanaged.passUnretained` pointer stays valid. Cancellation state is
/// lock-guarded because the cancel callback is polled from an engine thread
/// while `Task` cancellation is signalled from Swift concurrency.
final class RemeshControlBox {
    private let progressContinuation: AsyncStream<Double>.Continuation
    private let lock = NSLock()
    private var cancelled = false

    init(progressContinuation: AsyncStream<Double>.Continuation) {
        self.progressContinuation = progressContinuation
    }

    func reportProgress(_ value: Double) {
        progressContinuation.yield(value)
    }

    func finishProgress() {
        progressContinuation.finish()
    }

    func requestCancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }
}

// C function pointers must be context-free top-level closures; the engine's
// `user` pointer carries the RemeshControlBox back to us. The stage label the
// ABI also passes is unused here — progress is reported as a bare fraction.
private let remeshProgressCb: CyberProgressCb = { fraction, _stage, user in
    guard let user else { return }
    Unmanaged<RemeshControlBox>.fromOpaque(user)
        .takeUnretainedValue()
        .reportProgress(Double(fraction))
}

private let remeshCancelCb: CyberCancelCb = { user in
    guard let user else { return 0 }
    return Unmanaged<RemeshControlBox>.fromOpaque(user).takeUnretainedValue().isCancelled ? 1 : 0
}

/// A running remesh: observe ``progress`` while awaiting ``value()``.
///
/// ```swift
/// let op = mesh.remesh(params: .init(targetQuads: 5000))
/// Task { for await p in op.progress { updateBar(p) } }
/// let quadMesh = try await op.value()   // throws .cancelled if the Task is cancelled
/// ```
public final class RemeshOperation {
    /// Monotonic-ish progress in `0...1`; finishes when the operation ends.
    public let progress: AsyncStream<Double>

    private let input: Mesh
    private let params: RemeshParameters
    private let box: RemeshControlBox

    init(input: Mesh, params: RemeshParameters) {
        let (stream, continuation) = AsyncStream<Double>.makeStream(
            of: Double.self,
            bufferingPolicy: .bufferingNewest(1)
        )
        self.progress = stream
        self.input = input
        self.params = params
        self.box = RemeshControlBox(progressContinuation: continuation)
    }

    /// Awaits the remeshed result, bridging `Task` cancellation to the engine.
    ///
    /// - Throws: ``CyberError`` (`.cancelled` on cooperative cancellation).
    public func value() async throws -> Mesh {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<Mesh, Error>) in
                let input = self.input
                let params = self.params
                let box = self.box
                Thread.detachNewThread {
                    let result = RemeshOperation.run(input: input, params: params, box: box)
                    box.finishProgress()
                    continuation.resume(with: result)
                }
            }
        } onCancel: {
            self.box.requestCancel()
        }
    }

    /// Runs the blocking C call. Executed on a dedicated thread.
    private static func run(
        input: Mesh,
        params: RemeshParameters,
        box: RemeshControlBox
    ) -> Result<Mesh, Error> {
        var cparams = params.cValue
        var out: OpaquePointer?
        let user = Unmanaged.passUnretained(box).toOpaque()
        let status = cyber_remesh(
            input.handle, &cparams,
            remeshProgressCb, remeshCancelCb, user,
            &out
        )
        withExtendedLifetime(input) {}
        guard status == CYBER_OK, let handle = out else {
            return .failure(CyberError.map(status))
        }
        return .success(Mesh(owning: handle))
    }
}

public extension Mesh {
    /// Starts a remesh and returns the observable operation.
    func remesh(params: RemeshParameters) -> RemeshOperation {
        RemeshOperation(input: self, params: params)
    }

    /// Convenience: awaits the result while forwarding progress to a closure.
    ///
    /// - Throws: ``CyberError`` (`.cancelled` if the surrounding `Task` cancels).
    func remesh(
        params: RemeshParameters,
        onProgress: @escaping @Sendable (Double) -> Void
    ) async throws -> Mesh {
        let operation = remesh(params: params)
        let pump = Task {
            for await value in operation.progress {
                onProgress(value)
            }
        }
        defer { pump.cancel() }
        return try await operation.value()
    }
}
