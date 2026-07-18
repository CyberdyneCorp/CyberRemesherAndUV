// UNVERIFIED: iPadOS shell — root SwiftUI view (tasks 8.5 + 8.6). Requires the
// iPadOS SDK; compiles only inside an iOS app target, not with headless
// `swift build`. Composes the stage switcher, the Metal viewport host with the
// Pencil/touch feed overlaid, the configurable Action toolbar, the long-op
// progress/cancel overlay (atomic commit), and the in-app log view.

#if canImport(SwiftUI) && canImport(UIKit)
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var showLog = false

    var body: some View {
        NavigationStack {
            ZStack {
                viewport
                progressOverlay
            }
            .toolbar { toolbarContent }
            .navigationTitle("CyberRemesher")
            .navigationBarTitleDisplayMode(.inline)
            .sheet(isPresented: $showLog) {
                LogView(buffer: model.log)
            }
        }
    }

    // MARK: Viewport + input feed

    private var viewport: some View {
        ZStack {
            if let session = model.currentSession {
                // The engine renders into this CAMetalLayer host…
                MetalViewportView(session: session)
                    .ignoresSafeArea()
                // …and the Pencil/touch feed is captured on a transparent
                // overlay layered on top, so drawing and rendering share the
                // exact same coordinate frame.
                PencilCanvasView(session: session)
                    .ignoresSafeArea()
            } else {
                ContentUnavailablePlaceholder()
            }
        }
    }

    // MARK: Progress / cancel overlay (8.6)

    @ViewBuilder
    private var progressOverlay: some View {
        if model.operation.isBusy {
            ProgressOverlayView(state: model.operation) {
                model.cancelOperation()
            }
            .transition(.opacity)
        }
    }

    // MARK: Toolbars

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        // Stage switcher (Retopology → UV → Baking).
        ToolbarItem(placement: .principal) {
            Picker("Stage", selection: $model.selectedStageID) {
                ForEach(model.stages) { stage in
                    Label(stage.title, systemImage: stage.systemImage).tag(stage.id)
                }
            }
            .pickerStyle(.segmented)
        }
        // Configurable Action toolbar (data-driven, filtered by stage).
        ToolbarItemGroup(placement: .bottomBar) {
            ForEach(actionsForStage) { action in
                Button {
                    model.activeActionID = action.id
                } label: {
                    Label(action.title, systemImage: action.systemImage)
                }
                .tint(model.activeActionID == action.id ? .accentColor : .primary)
            }
            Spacer()
            Button {
                showLog = true
            } label: {
                Label("Log", systemImage: "list.bullet.rectangle")
            }
        }
    }

    private var actionsForStage: [ActionDescriptor] {
        model.toolbar.filter {
            $0.stages.isEmpty || $0.stages.contains(model.selectedStageID)
        }
    }
}

/// Shown before any document is opened.
private struct ContentUnavailablePlaceholder: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "square.and.arrow.down")
                .font(.largeTitle)
            Text("Import an OBJ to begin")
                .foregroundStyle(.secondary)
        }
    }
}
#endif // canImport(SwiftUI) && canImport(UIKit)
