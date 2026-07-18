// UNVERIFIED: iPadOS shell — in-app log view (task 8.6, quiet by default).
// Requires the iPadOS SDK; compiles only in an iOS app target. Renders the
// `LogBuffer.visible` entries with a level threshold the user can raise to
// `DEBUG` for diagnostics.

#if canImport(SwiftUI)
import SwiftUI

struct LogView: View {
    let buffer: LogBuffer
    @State private var showVerbose = false
    @Environment(\.dismiss) private var dismiss

    private var entries: [LogEntry] {
        let threshold: LogLevel = showVerbose ? .debug : .info
        return buffer.entries.filter { $0.level >= threshold }
    }

    var body: some View {
        NavigationStack {
            List(entries) { entry in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(entry.level.label)
                        .font(.caption2.monospaced())
                        .foregroundStyle(color(for: entry.level))
                        .frame(width: 52, alignment: .leading)
                    Text(entry.message)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                }
            }
            .listStyle(.plain)
            .navigationTitle("Log")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Toggle("Verbose", isOn: $showVerbose)
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func color(for level: LogLevel) -> Color {
        switch level {
        case .debug: return .secondary
        case .info: return .primary
        case .warn: return .orange
        case .error: return .red
        }
    }
}
#endif // canImport(SwiftUI)
