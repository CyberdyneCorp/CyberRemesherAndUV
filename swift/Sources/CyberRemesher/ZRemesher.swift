// ZRemesher-class retopology, at parity with the CLI and the Python bindings.
//
// `cyber_remesh` carries only the canonical `CyberRemeshParams`, so selecting
// `QuadMethod.zremesher` through it reaches the method but none of its
// controls, and returns none of what it found. This file binds the parity
// entry point instead — `cyber_remesh_zremesher`, which takes the ZRemesher
// parameters and mode-bearing guidance and fills a run report.
//
// The ABI grows by siblings rather than by new fields on shipped structs
// (`CyberRemeshParams` and `CyberFlowGuide` are passed as arrays that callers
// stride by `sizeof`), and this file mirrors that shape: nothing here changes
// `RemeshParameters` or `RemeshOperation`.

import CCyberRemesher
import Foundation

/// How hard the ZRemesher path works, mirroring `CYBER_ZR_QUALITY_*`.
public struct ZRemesherQuality: RawRepresentable, Equatable, Sendable {
    public var rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }

    /// Solve one predicted path.
    public static let fast = ZRemesherQuality(rawValue: Int32(CYBER_ZR_QUALITY_FAST))
    /// Solve BOTH cross-field candidates and keep the one that scores better.
    ///
    /// No static "organic vs CAD" threshold picks the right field for every
    /// model, so the answer is to measure both. It costs a second full solve.
    public static let best = ZRemesherQuality(rawValue: Int32(CYBER_ZR_QUALITY_BEST))
}

/// A forced symmetry plane, mirroring `CYBER_ZR_SYMMETRY_*`.
///
/// Symmetry is obtained by CONSTRUCTION: cut at the midplane, solve one half,
/// mirror the *connectivity*. A field solve on a symmetric surface is not a
/// symmetric function of it, so solving the whole mesh gives matching shape at
/// best while the halves carry different edge counts.
public struct SymmetryAxis: RawRepresentable, Equatable, Sendable {
    public var rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }

    public static let none = SymmetryAxis(rawValue: Int32(CYBER_ZR_SYMMETRY_NONE))
    public static let x = SymmetryAxis(rawValue: Int32(CYBER_ZR_SYMMETRY_X))
    public static let y = SymmetryAxis(rawValue: Int32(CYBER_ZR_SYMMETRY_Y))
    public static let z = SymmetryAxis(rawValue: Int32(CYBER_ZR_SYMMETRY_Z))
}

/// What a flow guide is asking for, mirroring `CYBER_GUIDE_*`.
public struct GuideMode: RawRepresentable, Equatable, Sendable {
    public var rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }

    /// "Line the field up this way around here" — a soft bias competing with
    /// the smoothness term. The default, and the historical behaviour.
    public static let orientation = GuideMode(rawValue: Int32(CYBER_GUIDE_ORIENTATION))
    /// "Put an actual edge loop HERE" — the stroke is projected to an edge path
    /// and pinned, so it becomes a curve in the layout rather than a bias on
    /// the field around it.
    public static let topology = GuideMode(rawValue: Int32(CYBER_GUIDE_TOPOLOGY))
}

/// A user-drawn flow guide: an ordered polyline on or near the input surface.
///
/// `points` is a flat `x,y,z` buffer, the same convention as
/// ``Mesh/positions()``, and needs at least two points. `radius` is a
/// world-space influence distance and must be greater than zero — a
/// zero-radius guide could never be honoured, so it is rejected rather than
/// silently ignored.
public struct FlowGuide: Sendable {
    /// Flat `x,y,z` triples; `count` must be a multiple of 3.
    public var points: [Float]
    /// Clamped to `0...1` by the engine.
    public var strength: Float
    /// World-space influence radius; must be > 0.
    public var radius: Float
    /// Whether the stroke biases the field or becomes an edge loop.
    public var mode: GuideMode
    /// The polyline closes back on its first point — an eye, mouth, shoulder,
    /// knee or wrist loop.
    public var closed: Bool

    public init(
        points: [Float],
        strength: Float = 1.0,
        radius: Float,
        mode: GuideMode = .orientation,
        closed: Bool = false
    ) {
        self.points = points
        self.strength = strength
        self.radius = radius
        self.mode = mode
        self.closed = closed
    }
}

/// Where a painted density field is indexed.
public enum DensityDomain: Sendable {
    /// One value per input vertex.
    case perVertex
    /// One value per input face.
    case perFace
}

/// User guidance for a run: flow guides, a painted density field, or both.
///
/// Density values are quads-per-unit-area multipliers clamped to `0.25...4.0`,
/// so the local edge length becomes `base / sqrt(density)`.
public struct Guidance: Sendable {
    public var guides: [FlowGuide]
    public var density: [Float]
    public var densityDomain: DensityDomain

    public init(
        guides: [FlowGuide] = [],
        density: [Float] = [],
        densityDomain: DensityDomain = .perVertex
    ) {
        self.guides = guides
        self.density = density
        self.densityDomain = densityDomain
    }

    public var isEmpty: Bool { guides.isEmpty && density.isEmpty }
}

/// The ZRemesher-only controls — a Swift mirror of `CyberZRemesherParams`.
///
/// The memberwise defaults come from the engine itself
/// (`cyber_default_zremesher_params`), so a freshly initialized value always
/// matches the CLI's behaviour.
public struct ZRemesherParameters: Sendable {
    /// One solve, or measure both cross-field candidates and keep the better.
    public var quality: ZRemesherQuality
    /// Solve one half and mirror its connectivity.
    ///
    /// ``RemeshParameters/targetQuads`` names the WHOLE model, so the half is
    /// solved for half of it.
    public var symmetry: SymmetryAxis
    /// Size the solve substrate from the unified sizing field.
    ///
    /// Off by default, and deliberately: it was MEASURED to make thin-feature
    /// survival worse, which is the one thing it exists to improve. Exposed so
    /// the measurement can be repeated, not because it is recommended.
    public var unifiedSizing: Bool
    /// Terminate separatrices reaching an open boundary there instead of
    /// abandoning them.
    public var boundaryChains: Bool
    /// Recover fold-damaged node rotations by feasible-range projection instead
    /// of containing the node.
    public var foldRepair: Bool

    /// The engine defaults, read back through the ABI.
    public init() {
        var defaults = CyberZRemesherParams()
        cyber_default_zremesher_params(&defaults)
        quality = ZRemesherQuality(rawValue: defaults.quality)
        symmetry = SymmetryAxis(rawValue: defaults.symmetry)
        unifiedSizing = defaults.unifiedSizing != 0
        boundaryChains = defaults.boundaryChains != 0
        foldRepair = defaults.foldRepair != 0
    }

    /// The engine defaults with a quality mode and symmetry axis applied.
    public init(quality: ZRemesherQuality = .fast, symmetry: SymmetryAxis = .none) {
        self.init()
        self.quality = quality
        self.symmetry = symmetry
    }

    var cValue: CyberZRemesherParams {
        CyberZRemesherParams(
            quality: quality.rawValue,
            symmetry: symmetry.rawValue,
            unifiedSizing: unifiedSizing ? 1 : 0,
            boundaryChains: boundaryChains ? 1 : 0,
            foldRepair: foldRepair ? 1 : 0
        )
    }
}

/// What a ZRemesher run produced — a Swift mirror of `CyberZRemesherReport`.
///
/// Layout counts are SUMMED over the islands a run solved: a multi-island mesh
/// has no single layout, and "how many singularities did this produce" means
/// all of them.
public struct ZRemesherReport: Sendable, Equatable {
    /// Topology layouts traced, and how many passed validation. A run where
    /// these differ produced output from a layout that failed its own
    /// invariants, and deserves inspection.
    public var layouts: Int
    public var layoutsValid: Int
    public var nodes: Int
    public var arcs: Int
    public var patches: Int
    public var singularities: Int
    public var tJunctions: Int
    public var featureArcs: Int
    public var boundaryArcs: Int
    /// Arcs the tracer contained, and patches whose boundary walk did not
    /// close. Both are containment, not failure: the sound remainder still
    /// produced the mesh.
    public var excludedArcs: Int
    public var nonClosingPatches: Int
    /// Sum of singularity indices; `4 * Euler characteristic` for a valid field.
    public var totalIndex: Int

    /// The cross field ``ZRemesherQuality/best`` kept (`"multires"` or
    /// `"single-level"`), and its score. Empty under ``ZRemesherQuality/fast``,
    /// which solves one predicted path and therefore selects nothing — an empty
    /// name means "no selection ran", never "selection failed".
    public var selectedCandidate: String
    public var qualityScore: Double

    /// False when no symmetry was requested.
    public var symmetryApplied: Bool
    /// Checked on the RESULT rather than assumed from the construction, because
    /// the construction is what a regression would break.
    public var topologicallySymmetric: Bool
    public var mirroredVertices: Int
    public var mirroredFaces: Int
    public var borderSnapped: Int
    public var membranesRemoved: Int
    public var maxBorderDrift: Float

    init(_ c: CyberZRemesherReport) {
        layouts = Int(c.layouts)
        layoutsValid = Int(c.layoutsValid)
        nodes = Int(c.layoutNodes)
        arcs = Int(c.layoutArcs)
        patches = Int(c.layoutPatches)
        singularities = Int(c.singularities)
        tJunctions = Int(c.tJunctions)
        featureArcs = Int(c.featureArcs)
        boundaryArcs = Int(c.boundaryArcs)
        excludedArcs = Int(c.excludedArcs)
        nonClosingPatches = Int(c.nonClosingPatches)
        totalIndex = Int(c.totalIndex)
        // `char[32]` imports as a 32-element tuple; read it as bytes and stop
        // at the NUL rather than assuming the whole buffer is the string.
        var candidate = c.selectedCandidate
        selectedCandidate = withUnsafeBytes(of: &candidate) { raw in
            let bytes = raw.prefix(while: { $0 != 0 })
            return String(decoding: bytes, as: UTF8.self)
        }
        qualityScore = c.qualityScore
        symmetryApplied = c.symmetryApplied != 0
        topologicallySymmetric = c.topologicallySymmetric != 0
        mirroredVertices = Int(c.mirroredVertices)
        mirroredFaces = Int(c.mirroredFaces)
        borderSnapped = Int(c.borderSnapped)
        membranesRemoved = Int(c.membranesRemoved)
        maxBorderDrift = c.maxBorderDrift
    }
}

/// A remeshed mesh plus what the ZRemesher path found producing it.
///
/// Deliberately NOT `Sendable`: it holds a ``Mesh``, which owns an opaque
/// engine handle and makes no thread-safety promise, so claiming `Sendable`
/// here would be a lie the compiler rejects outright under the Swift 6
/// language mode. ``ZRemesherReport`` is a plain value type and IS `Sendable`,
/// so the measurements can cross an isolation boundary on their own.
public struct ZRemesherResult {
    public let mesh: Mesh
    public let report: ZRemesherReport
    /// Guidance the engine could not honour, one message per island that
    /// declined. Never dropped silently.
    public let guidanceWarnings: [String]
}

/// Collects the ABI's warning callbacks for the duration of one call.
///
/// Separate from ``RemeshControlBox`` because warnings arrive on the engine
/// thread while the box's cancellation flag is written from Swift concurrency;
/// merging them would widen that lock's job for no reason.
final class GuidanceWarningBox {
    private let lock = NSLock()
    private var messages: [String] = []

    func append(_ message: String) {
        lock.lock()
        messages.append(message)
        lock.unlock()
    }

    var collected: [String] {
        lock.lock()
        defer { lock.unlock() }
        return messages
    }
}

/// Paired with the control box through the single opaque `user` pointer the
/// ABI shares between progress, cancel and warning callbacks.
final class ZRemesherCallbackBox {
    let control: RemeshControlBox
    let warnings: GuidanceWarningBox

    init(control: RemeshControlBox, warnings: GuidanceWarningBox) {
        self.control = control
        self.warnings = warnings
    }
}

private let zremesherProgressCb: CyberProgressCb = { fraction, _stage, user in
    guard let user else { return }
    Unmanaged<ZRemesherCallbackBox>.fromOpaque(user)
        .takeUnretainedValue()
        .control
        .reportProgress(Double(fraction))
}

private let zremesherCancelCb: CyberCancelCb = { user in
    guard let user else { return 0 }
    let box = Unmanaged<ZRemesherCallbackBox>.fromOpaque(user).takeUnretainedValue()
    return box.control.isCancelled ? 1 : 0
}

private let zremesherWarningCb: CyberWarningCb = { message, user in
    guard let user, let message else { return }
    Unmanaged<ZRemesherCallbackBox>.fromOpaque(user)
        .takeUnretainedValue()
        .warnings
        .append(String(cString: message))
}

/// A running ZRemesher operation: observe ``progress`` while awaiting
/// ``value()``.
public final class ZRemesherOperation {
    /// Monotonic-ish progress in `0...1`; finishes when the operation ends.
    public let progress: AsyncStream<Double>

    private let input: Mesh
    private let params: RemeshParameters
    private let zremesher: ZRemesherParameters
    private let guidance: Guidance
    private let control: RemeshControlBox

    init(
        input: Mesh,
        params: RemeshParameters,
        zremesher: ZRemesherParameters,
        guidance: Guidance
    ) {
        let (stream, continuation) = AsyncStream<Double>.makeStream(
            of: Double.self,
            bufferingPolicy: .bufferingNewest(1)
        )
        self.progress = stream
        self.input = input
        self.params = params
        self.zremesher = zremesher
        self.guidance = guidance
        self.control = RemeshControlBox(progressContinuation: continuation)
    }

    /// Awaits the result, bridging `Task` cancellation to the engine.
    ///
    /// - Throws: ``CyberError`` (`.cancelled` on cooperative cancellation,
    ///   `.invalidParameter` for an unknown quality mode, symmetry axis or
    ///   guide mode — those are rejected, never reinterpreted).
    public func value() async throws -> ZRemesherResult {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<ZRemesherResult, Error>) in
                let input = self.input
                let params = self.params
                let zremesher = self.zremesher
                let guidance = self.guidance
                let control = self.control
                Thread.detachNewThread {
                    let result = ZRemesherOperation.run(
                        input: input, params: params,
                        zremesher: zremesher, guidance: guidance, control: control
                    )
                    control.finishProgress()
                    continuation.resume(with: result)
                }
            }
        } onCancel: {
            self.control.requestCancel()
        }
    }

    /// Runs the blocking C call on a dedicated thread.
    private static func run(
        input: Mesh,
        params: RemeshParameters,
        zremesher: ZRemesherParameters,
        guidance: Guidance,
        control: RemeshControlBox
    ) -> Result<ZRemesherResult, Error> {
        // Validated here rather than in C, for the same reason the Python
        // binding does it: the ABI reads `3 * point_count` floats through a
        // pointer with an implied length, so a stroke that is not whole xyz
        // triples would be read past the end of its own buffer.
        for (index, guide) in guidance.guides.enumerated() {
            guard guide.points.count % 3 == 0 else {
                return .failure(
                    CyberError.invalidArgument(
                        "guide \(index): points count must be a multiple of 3 (x, y, z), "
                            + "got \(guide.points.count)"
                    )
                )
            }
        }

        let warnings = GuidanceWarningBox()
        let box = ZRemesherCallbackBox(control: control, warnings: warnings)
        let user = Unmanaged.passUnretained(box).toOpaque()

        var cparams = params.cValue
        var czr = zremesher.cValue
        var creport = CyberZRemesherReport()
        var out: OpaquePointer?

        // Every point buffer has to stay alive across the call, so the guides
        // are built inside nested `withUnsafeBufferPointer` scopes rather than
        // from temporaries that would be gone by the time the engine reads them.
        let status = withGuidance(guidance) { guidancePtr in
            cyber_remesh_zremesher(
                input.handle, &cparams, &czr, guidancePtr,
                zremesherProgressCb, zremesherCancelCb, zremesherWarningCb, user,
                &out, &creport
            )
        }
        withExtendedLifetime(input) {}
        withExtendedLifetime(box) {}

        guard status == CYBER_OK, let handle = out else {
            return .failure(CyberError.map(status))
        }
        return .success(
            ZRemesherResult(
                mesh: Mesh(owning: handle),
                report: ZRemesherReport(creport),
                guidanceWarnings: warnings.collected
            )
        )
    }

    /// Lowers `Guidance` to a `CyberGuidanceEx` valid only inside `body`.
    ///
    /// Recursive over the guides so each stroke's buffer is pinned by its own
    /// scope; the base case builds the guidance struct and calls through. A
    /// guidance-free run passes NULL, which the ABI documents as identical to
    /// the unguided path.
    private static func withGuidance<R>(
        _ guidance: Guidance,
        _ body: (UnsafePointer<CyberGuidanceEx>?) -> R
    ) -> R {
        guard !guidance.isEmpty else { return body(nil) }

        var cguides: [CyberFlowGuideEx] = []
        cguides.reserveCapacity(guidance.guides.count)

        func pin(_ index: Int) -> R {
            guard index < guidance.guides.count else {
                return guidance.density.withUnsafeBufferPointer { density in
                    cguides.withUnsafeBufferPointer { guides in
                        var c = CyberGuidanceEx()
                        c.guides = guides.baseAddress
                        c.guide_count = guides.count
                        let base = guidance.density.isEmpty ? nil : density.baseAddress
                        switch guidance.densityDomain {
                        case .perVertex:
                            c.vertex_density = base
                            c.vertex_density_count = guidance.density.count
                        case .perFace:
                            c.face_density = base
                            c.face_density_count = guidance.density.count
                        }
                        return withUnsafePointer(to: &c) { body($0) }
                    }
                }
            }
            let guide = guidance.guides[index]
            return guide.points.withUnsafeBufferPointer { points in
                var c = CyberFlowGuideEx()
                c.points = points.baseAddress
                c.point_count = points.count / 3
                c.strength = guide.strength
                c.radius = guide.radius
                c.mode = guide.mode.rawValue
                c.closed = guide.closed ? 1 : 0
                cguides.append(c)
                return pin(index + 1)
            }
        }

        return pin(0)
    }
}

public extension Mesh {
    /// Starts a ZRemesher-class retopology and returns the observable operation.
    ///
    /// This is the parity entry point: every ZRemesher capability the headless
    /// CLI offers is reachable here. `params.quadMethod` is ignored — this call
    /// *is* the ZRemesher method — but every other field applies, adaptivity
    /// included.
    func remesh(
        params: RemeshParameters,
        zremesher: ZRemesherParameters,
        guidance: Guidance = Guidance()
    ) -> ZRemesherOperation {
        ZRemesherOperation(
            input: self, params: params, zremesher: zremesher, guidance: guidance
        )
    }

    /// Convenience: awaits the result while forwarding progress to a closure.
    ///
    /// - Throws: ``CyberError`` (`.cancelled` if the surrounding `Task` cancels).
    func remesh(
        params: RemeshParameters,
        zremesher: ZRemesherParameters,
        guidance: Guidance = Guidance(),
        onProgress: @escaping @Sendable (Double) -> Void
    ) async throws -> ZRemesherResult {
        let operation = remesh(params: params, zremesher: zremesher, guidance: guidance)
        let pump = Task {
            for await value in operation.progress {
                onProgress(value)
            }
        }
        defer { pump.cancel() }
        return try await operation.value()
    }
}
