// UNVERIFIED: iPadOS shell — long-op progress/cancel overlay (task 8.6).
// Requires the iPadOS SDK; compiles only in an iOS app target. Renders the
// `LongOperationState` and offers Cancel. The atomic-commit guarantee lives in
// `AppModel` (the result swaps in one step); this view is a faithful readout
// and never mutates the document itself, so it can't cause a stale flash.

#if canImport(SwiftUI)
import SwiftUI

struct ProgressOverlayView: View {
    let state: LongOperationState
    let onCancel: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.35).ignoresSafeArea()
            VStack(spacing: 16) {
                switch state {
                case .running(let progress, let label):
                    Text(label).font(.headline)
                    ProgressView(value: progress)
                        .progressViewStyle(.linear)
                        .frame(width: 260)
                    Text("\(Int(progress * 100))%")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                    Button(role: .cancel, action: onCancel) {
                        Text("Cancel")
                    }
                    .buttonStyle(.bordered)
                case .cancelling:
                    ProgressView()
                    Text("Cancelling…").font(.headline)
                case .idle:
                    EmptyView()
                }
            }
            .padding(28)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
        }
        .animation(.easeInOut(duration: 0.15), value: state)
    }
}
#endif // canImport(SwiftUI)
