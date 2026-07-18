// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the ABI contract in README.md.
//
// Async/await remeshing. The engine's `cyber_remesh` is a blocking C call that
// takes C function-pointer callbacks for progress and cancellation. This file
// bridges it to Swift concurrency:
//   * progress -> an `AsyncStream<Double>` the caller can `for await` over;
//   * Swift `Task` cancellation -> the ABI's `CyberCancelCb`, which the engine
//     polls cooperatively (< 100 ms) and unwinds leaving inputs untouched.
// The blocking call runs on a dedicated thread so it never stalls the Swift
// cooperative thread pool.

import CCyberRemesher
import Foundation

/// User-facing remeshing parameters, mapped onto the ABI's opaque params object.
public struct RemeshParameters: Sendable {
    /// Desired output quad count. Clamped to a safe minimum by the engine
    /// (guards the historic `--target-quads 0` division-by-zero defect).
    public var targetQuads: Int
    /// Request the pure-quad post-pass (topological cleanup of non-quads).
    public var pureQuad: Bool
    /// Preserve sharp feature edges during the flow.
    public var preserveSharpEdges: Bool
    /// Dihedral angle (degrees) above which an edge counts as sharp.
    public var sharpAngleDegrees: Double

    public init(
        targetQuads: Int,
        pureQuad: Bool = false,
        preserveSharpEdges: Bool = true,
        sharpAngleDegrees: Double = 30.0
    ) {
        self.targetQuads = targetQuads
        self.pureQuad = pureQuad
        self.preserveSharpEdges = preserveSharpEdges
        self.sharpAngleDegrees = sharpAngleDegrees
    }

    /// Pushes the values onto an engine-owned params handle.
    func apply(to handle: OpaquePointer) {
        cyber_remesh_params_set_target_quads(handle, max(0, targetQuads))
        cyber_remesh_params_set_pure_quad(handle, pureQuad ? 1 : 0)
        cyber_remesh_params_set_preserve_sharp(handle, preserveSharpEdges ? 1 : 0)
        cyber_remesh_params_set_sharp_angle(handle, sharpAngleDegrees)
    }
}

/// Shared control block handed to the C callbacks via an opaque `user` pointer.
///
/// Held strongly by the running thread closure for the whole blocking call, so
/// the `Unmanaged.passUnretained` pointers stay valid. Cancellation state is
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
// `user` pointer carries the RemeshControlBox back to us.
private let remeshProgressCb: CyberProgressCb = { progress, _stage, user in
    guard let user else { return }
    Unmanaged<RemeshControlBox>.fromOpaque(user).takeUnretainedValue().reportProgress(progress)
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
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Mesh, Error>) in
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
        guard let cparams = cyber_remesh_params_create() else {
            return .failure(CyberError.outOfMemory)
        }
        defer { cyber_remesh_params_destroy(cparams) }
        params.apply(to: cparams)

        var out: OpaquePointer?
        let user = Unmanaged.passUnretained(box).toOpaque()
        let status = cyber_remesh(
            input.handle, cparams,
            remeshProgressCb, user,
            remeshCancelCb, user,
            &out
        )
        withExtendedLifetime(input) {}
        guard status == CYBER_STATUS_OK, let handle = out else {
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
