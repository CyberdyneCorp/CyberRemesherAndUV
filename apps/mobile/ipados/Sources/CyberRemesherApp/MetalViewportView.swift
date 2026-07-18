// UNVERIFIED: iPadOS shell — CAMetalLayer viewport host (task 8.5). Requires
// UIKit/QuartzCore/Metal; compiles only in an iOS app target. Bridges a
// `CAMetalLayer`-backed UIView to the package's `MetalViewport`, which hands the
// layer to the engine render backend. The shell owns the layer's lifecycle and
// forwards drawable-size changes; frame scheduling is the engine's.

#if canImport(SwiftUI) && canImport(UIKit) && canImport(Metal)
import SwiftUI
import UIKit
import QuartzCore
import CyberRemesher

/// A `UIView` whose backing layer is a `CAMetalLayer` — the surface the engine
/// renders into.
final class MetalHostView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer } // swiftlint:disable:this force_cast

    private var viewport: MetalViewport?
    private var session: Session?

    func bind(session: Session) {
        guard self.session !== session else { return }
        teardown()
        let viewport = MetalViewport(session: session, layer: metalLayer)
        do {
            try viewport.attach()
            self.viewport = viewport
            self.session = session
        } catch {
            // Attach can fail on a build with no Metal device (e.g. simulator
            // without GPU); leave the host blank rather than crashing.
            self.viewport = nil
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        // Report the pixel-accurate drawable size on every layout pass so the
        // engine's swapchain matches the view (rotation, split-view resize…).
        let scale = window?.screen.nativeScale ?? contentScaleFactor
        let size = bounds.size
        try? viewport?.setDrawableSize(
            width: Double(size.width * scale),
            height: Double(size.height * scale)
        )
    }

    private func teardown() {
        try? viewport?.detach()
        viewport = nil
        session = nil
    }

    deinit { teardown() }
}

/// SwiftUI wrapper for `MetalHostView`.
struct MetalViewportView: UIViewRepresentable {
    let session: Session

    func makeUIView(context: Context) -> MetalHostView {
        let view = MetalHostView(frame: .zero)
        view.bind(session: session)
        return view
    }

    func updateUIView(_ view: MetalHostView, context: Context) {
        view.bind(session: session)
    }
}
#endif // canImport(SwiftUI) && canImport(UIKit) && canImport(Metal)
