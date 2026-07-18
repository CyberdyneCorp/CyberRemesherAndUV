// UNVERIFIED: iPadOS shell view model. Requires the CyberRemesher Swift package
// + linked engine; the type is UIKit-free (Foundation/Combine only) so it
// type-checks with `swift build`. Owns the document/session, the stage state,
// the autosave hook (8.5), and the long-op progress/cancel state machine with
// atomic commit (8.6).

import Foundation
import Combine
import CyberRemesher

/// The single-window document model. Everything the SwiftUI tree observes lives
/// here; the views are otherwise stateless.
@MainActor
public final class AppModel: ObservableObject {
    // MARK: Data-driven UI model (8.6)
    @Published public private(set) var stages: [Stage]
    @Published public private(set) var toolbar: [ActionDescriptor]
    @Published public var selectedStageID: String
    @Published public var activeActionID: String?

    // MARK: Long-operation state (8.6, atomic commit)
    @Published public private(set) var operation: LongOperationState = .idle

    // MARK: In-app log (8.6, quiet by default)
    @Published public private(set) var log = LogBuffer()

    // MARK: Document
    private var session: Session?
    private var displayedMesh: Mesh?
    private var runningOp: RemeshOperation?
    private var progressPump: Task<Void, Never>?

    private let autosaveURL: URL

    public init() {
        let stageModel = UIModelLoader.loadStages()
        let toolbarModel = UIModelLoader.loadToolbar()
        let byID = Dictionary(uniqueKeysWithValues:
            toolbarModel.actions.map { ($0.id, $0) })
        self.stages = stageModel.stages
        self.toolbar = toolbarModel.defaultToolbar.compactMap { byID[$0] }
        self.selectedStageID = stageModel.stages.first?.id ?? "retopology"
        self.autosaveURL = Self.defaultAutosaveURL()
        self.log.info("shell ready — engine \(CyberRuntime.version)")
    }

    // MARK: - Document lifecycle

    /// Opens a document over `mesh`, replacing any current session.
    public func open(mesh: Mesh) {
        do {
            let session = try Session(mesh: mesh)
            self.session = session
            self.displayedMesh = mesh
            log.info("opened document (\(mesh.vertexCount) verts)")
        } catch {
            log.error("open failed: \(error)")
        }
    }

    /// The live session for viewport/input wiring; nil until a document opens.
    public var currentSession: Session? { session }

    // MARK: - Backgrounding autosave (8.5)

    /// Called from `ScenePhase` → `.background`/`.inactive`. Must be quick and
    /// synchronous: the OS may suspend us immediately after.
    public func autosaveOnResignActive() {
        guard let session else { return }
        do {
            let snapshot = try session.snapshot()
            try snapshot.save(to: autosaveURL.path)
            log.info("autosaved to \(autosaveURL.lastPathComponent)")
        } catch {
            log.error("autosave failed: \(error)")
        }
    }

    /// Called when returning to the foreground.
    public func resume() {
        log.debug("resumed")
    }

    // MARK: - Long operation with atomic commit (8.6)

    /// Runs a remesh. The viewport keeps showing the *current* mesh for the
    /// whole run; the new mesh is swapped in a single atomic step only on
    /// success — never a half-updated frame (no stale flash). Cancel leaves the
    /// document exactly as it was.
    public func runRemesh(_ params: RemeshParameters) {
        guard case .idle = operation, let source = displayedMesh else { return }
        operation = .running(progress: 0, label: "Remeshing")
        log.info("remesh started (target \(params.targetQuads) quads)")

        let op = source.remesh(params: params)
        runningOp = op
        progressPump = Task { [weak self] in
            for await p in op.progress {
                await self?.updateProgress(p)
            }
        }

        Task { [weak self] in
            guard let self else { return }
            do {
                let result = try await op.value()
                // Atomic commit: build the new session first, then publish the
                // new mesh + reset op in one main-actor hop. If session
                // creation throws we keep the old document intact.
                let newSession = try Session(mesh: result)
                self.commit(mesh: result, session: newSession)
            } catch CyberError.cancelled {
                self.rollback(reason: "cancelled")
            } catch is CancellationError {
                self.rollback(reason: "cancelled")
            } catch {
                self.rollback(reason: "\(error)")
            }
        }
    }

    /// Requests cooperative cancellation of the running operation.
    public func cancelOperation() {
        guard case .running = operation else { return }
        operation = .cancelling
        progressPump?.cancel() // bridges to the engine's CyberCancelCb
        log.info("cancel requested")
    }

    private func updateProgress(_ value: Double) {
        guard case .running(_, let label) = operation else { return }
        operation = .running(progress: value, label: label)
    }

    private func commit(mesh: Mesh, session: Session) {
        self.displayedMesh = mesh
        self.session = session
        self.runningOp = nil
        self.operation = .idle
        log.info("remesh committed (\(mesh.faceCount) faces)")
    }

    private func rollback(reason: String) {
        self.runningOp = nil
        self.operation = .idle
        log.warn("remesh ended: \(reason) (document unchanged)")
    }

    // MARK: - Helpers

    private static func defaultAutosaveURL() -> URL {
        let dir = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask
        ).first ?? FileManager.default.temporaryDirectory
        return dir.appendingPathComponent("autosave.obj")
    }
}

/// The long-operation state the progress overlay renders (8.6). Modeled so the
/// UI can never show a partially-applied result: it is either `.idle` (showing
/// the committed document) or busy — the swap happens atomically in `AppModel`.
public enum LongOperationState: Equatable {
    case idle
    case running(progress: Double, label: String)
    case cancelling

    public var isBusy: Bool {
        if case .idle = self { return false }
        return true
    }
}
