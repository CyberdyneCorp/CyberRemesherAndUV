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
    /// ZRemesher-class retopology: structurally the quad-cover path with the
    /// explicit topology-layout stage on.
    ///
    /// Selecting it here runs it through plain `cyber_remesh`, which carries no
    /// ZRemesher controls and returns no run report. To reach the quality mode,
    /// the symmetry axis, guide modes or the layout statistics, use
    /// ``Mesh/remesh(params:zremesher:guidance:)`` instead — that is the
    /// entry point the CLI and the other bindings are at parity with.
    public static let zremesher = QuadMethod(rawValue: Int32(CYBER_QUAD_ZREMESHER))
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

/// Optional bounds for target-count calibration. Omitting this policy keeps
/// the engine's historical default calibration behavior.
public struct CountPolicy: Sendable {
    public var relativeTolerance: Double
    public var maxAttempts: Int

    public init(relativeTolerance: Double, maxAttempts: Int) {
        self.relativeTolerance = relativeTolerance
        self.maxAttempts = maxAttempts
    }
}

public struct CountIslandOutcome: Sendable {
    public let islandIndex: Int
    public let requestedQuads: Double
    public let effectiveBaseQuads: Double
    public let calibratedQuads: Double
    public let finalFaces: Int
    public let attempts: Int
    public let selectedAttempt: Int
    public let termination: Int32
}

public struct TargetCountReport: Sendable {
    public let requestedQuads: Int
    public let effectiveBaseQuads: Int
    public let finalFaces: Int
    public let pureQuads: Bool
    public let islands: [CountIslandOutcome]
}

/// Exact opt-in topology ceilings for one plain remesh call. Zero disables a
/// dimension. They constrain mesh element counts, not process RSS.
public struct RemeshLimits: Sendable {
    public var maxInputVertices: UInt64 = 0
    public var maxInputFaces: UInt64 = 0
    public var maxIntermediateVertices: UInt64 = 0
    public var maxIntermediateFaces: UInt64 = 0
    public var maxOutputVertices: UInt64 = 0
    public var maxOutputFaces: UInt64 = 0
    public var maxDirectFactorBytes: UInt64 = 0
    public var maxCandidateBytes: UInt64 = 0

    public init() {}

    var cValue: CyberRemeshLimits {
        CyberRemeshLimits(maxInputVertices: maxInputVertices, maxInputFaces: maxInputFaces,
                           maxIntermediateVertices: maxIntermediateVertices,
                           maxIntermediateFaces: maxIntermediateFaces,
                           maxOutputVertices: maxOutputVertices, maxOutputFaces: maxOutputFaces)
    }

    var executionCValue: CyberRemeshExecutionLimits {
        CyberRemeshExecutionLimits(maxDirectFactorBytes: maxDirectFactorBytes,
                                   maxCandidateBytes: maxCandidateBytes)
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
let remeshProgressCb: CyberProgressCb = { fraction, _stage, user in
    guard let user else { return }
    Unmanaged<RemeshControlBox>.fromOpaque(user)
        .takeUnretainedValue()
        .reportProgress(Double(fraction))
}

let remeshCancelCb: CyberCancelCb = { user in
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
    private let limits: RemeshLimits?
    private let box: RemeshControlBox

    init(input: Mesh, params: RemeshParameters, limits: RemeshLimits?) {
        let (stream, continuation) = AsyncStream<Double>.makeStream(
            of: Double.self,
            bufferingPolicy: .bufferingNewest(1)
        )
        self.progress = stream
        self.input = input
        self.params = params
        self.limits = limits
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
                let limits = self.limits
                let box = self.box
                Thread.detachNewThread {
                    let result = RemeshOperation.run(input: input, params: params, limits: limits, box: box)
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
        limits: RemeshLimits?,
        box: RemeshControlBox
    ) -> Result<Mesh, Error> {
        var cparams = params.cValue
        var out: OpaquePointer?
        let user = Unmanaged.passUnretained(box).toOpaque()
        let status: CyberStatus
        if let limits {
            var cLimits = limits.cValue
            status = cyber_remesh_with_limits(input.handle, &cparams, &cLimits,
                                              remeshProgressCb, remeshCancelCb, user, &out)
        } else {
            status = cyber_remesh(input.handle, &cparams,
                                  remeshProgressCb, remeshCancelCb, user, &out)
        }
        withExtendedLifetime(input) {}
        guard status == CYBER_OK, let handle = out else {
            return .failure(CyberError.map(status))
        }
        return .success(Mesh(owning: handle))
    }
}

public extension Mesh {
    /// Runs remeshing with an explicit target-count policy and returns the
    /// caller-visible calibration outcome alongside the result mesh.
    func remeshWithCountReport(
        params: RemeshParameters,
        countPolicy: CountPolicy
    ) throws -> (mesh: Mesh, report: TargetCountReport) {
        var cparams = params.cValue
        var cpolicy = CyberCountPolicy(
            relativeTolerance: countPolicy.relativeTolerance,
            maxAttempts: numericCast(countPolicy.maxAttempts)
        )
        var output: OpaquePointer?
        let capacity = max(1, Int(cyber_mesh_face_count(handle)))
        var islands = Array(repeating: CyberCountIslandOutcome(), count: capacity)
        var report = CyberTargetCountReport()
        let status = islands.withUnsafeMutableBufferPointer { buffer in
            report.islands = buffer.baseAddress
            report.islandCapacity = buffer.count
            return cyber_remesh_with_count_report(
                handle, &cparams, &cpolicy, nil, nil, nil, &output, &report
            )
        }
        guard status == CYBER_OK, let output else { throw CyberError.map(status) }
        let outcomes = islands.prefix(Int(report.islandCount)).map { row in
            CountIslandOutcome(
                islandIndex: Int(row.islandIndex), requestedQuads: row.requestedQuads,
                effectiveBaseQuads: row.effectiveBaseQuads, calibratedQuads: row.calibratedQuads,
                finalFaces: Int(row.finalFaces), attempts: Int(row.attempts),
                selectedAttempt: Int(row.selectedAttempt), termination: row.termination
            )
        }
        return (Mesh(owning: output), TargetCountReport(
            requestedQuads: Int(report.requestedQuads), effectiveBaseQuads: Int(report.effectiveBaseQuads),
            finalFaces: Int(report.finalFaces), pureQuads: report.pureQuads != 0, islands: outcomes
        ))
    }

    /// Starts a remesh and returns the observable operation.
    func remesh(params: RemeshParameters, limits: RemeshLimits? = nil) -> RemeshOperation {
        RemeshOperation(input: self, params: params, limits: limits)
    }

    /// Convenience: awaits the result while forwarding progress to a closure.
    ///
    /// - Throws: ``CyberError`` (`.cancelled` if the surrounding `Task` cancels).
    func remesh(
        params: RemeshParameters,
        limits: RemeshLimits? = nil,
        onProgress: @escaping @Sendable (Double) -> Void
    ) async throws -> Mesh {
        let operation = remesh(params: params, limits: limits)
        let pump = Task {
            for await value in operation.progress {
                onProgress(value)
            }
        }
        defer { pump.cancel() }
        return try await operation.value()
    }
}
