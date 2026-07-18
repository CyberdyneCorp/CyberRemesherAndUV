// UNVERIFIED: iPadOS SwiftUI app entry. Requires Xcode 15+/iPadOS 15+ and the
// linked `cyber_capi` library; not buildable in headless Linux CI. Consumes the
// first-party CyberRemesher Swift package (no private hooks).
//
// App entry + lifecycle. The single responsibility here is to own the
// `AppModel` and translate SwiftUI's `ScenePhase` into the engine's
// backgrounding-autosave hook (task 8.5). All other behaviour lives in child
// views and the model.

#if canImport(SwiftUI)
import SwiftUI

@main
struct CyberRemesherApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
        .onChange(of: scenePhase) { phase in
            // Backgrounding autosave: iPadOS gives an app a short, best-effort
            // window when it leaves the foreground. Snapshot + persist there so
            // an OS-initiated suspension never loses in-flight edits.
            switch phase {
            case .background, .inactive:
                model.autosaveOnResignActive()
            case .active:
                model.resume()
            @unknown default:
                break
            }
        }
    }
}
#endif // canImport(SwiftUI)
