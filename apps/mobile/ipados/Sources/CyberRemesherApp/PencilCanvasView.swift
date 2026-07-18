// UNVERIFIED: iPadOS shell — Pencil/touch event feed (task 8.5). Requires
// UIKit/PencilKit; compiles only in an iOS app target. Captures native touches
// and forwards them into the engine through the package's
// `TouchInputForwarder`. It makes no routing decisions: fingers vs Pencil are
// tagged and handed to the engine, which decides navigate-vs-draw (chording).

#if canImport(SwiftUI) && canImport(UIKit)
import SwiftUI
import UIKit
import CyberRemesher

/// A transparent `UIView` overlaid on the viewport that turns raw touch phases
/// into engine strokes.
final class InputCaptureView: UIView {
    private var forwarder: TouchInputForwarder?
    private var session: Session?

    func bind(session: Session) {
        guard self.session !== session else { return }
        self.session = session
        self.forwarder = TouchInputForwarder(session: session, view: self)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwarder?.begin()
        forwarder?.append(touches, coalescedFrom: event)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwarder?.append(touches, coalescedFrom: event)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwarder?.append(touches, coalescedFrom: event)
        try? forwarder?.end()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        // Drop the in-flight stroke; the engine never sees a cancelled gesture.
        forwarder?.begin()
    }
}

/// SwiftUI wrapper for `InputCaptureView`.
struct PencilCanvasView: UIViewRepresentable {
    let session: Session

    func makeUIView(context: Context) -> InputCaptureView {
        let view = InputCaptureView(frame: .zero)
        view.bind(session: session)
        return view
    }

    func updateUIView(_ view: InputCaptureView, context: Context) {
        view.bind(session: session)
    }
}
#endif // canImport(SwiftUI) && canImport(UIKit)
