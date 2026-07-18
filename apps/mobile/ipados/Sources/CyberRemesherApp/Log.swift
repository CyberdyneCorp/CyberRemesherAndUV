// UNVERIFIED: iPadOS shell — in-app log model (task 8.6, "quiet by default").
// Pure Swift + Foundation; type-checks with `swift build`. A bounded ring of
// structured entries the in-app Log view renders. Quiet by default: only
// `.info`/`.warn`/`.error` show unless the user enables verbose (`.debug`).

import Foundation

/// Severity of an in-app log entry. Ordered so a threshold filter is a `>=`.
public enum LogLevel: Int, Codable, CaseIterable, Comparable, Sendable {
    case debug = 0, info, warn, error

    public static func < (lhs: LogLevel, rhs: LogLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    public var label: String {
        switch self {
        case .debug: return "DEBUG"
        case .info: return "INFO"
        case .warn: return "WARN"
        case .error: return "ERROR"
        }
    }
}

/// One log line.
public struct LogEntry: Identifiable, Equatable, Sendable {
    public let id = UUID()
    public let time: Date
    public let level: LogLevel
    public let message: String
}

/// A bounded, observable log. Kept tiny (a ring buffer) so it never grows
/// unbounded during a long editing session.
public struct LogBuffer: Equatable, Sendable {
    public private(set) var entries: [LogEntry] = []
    /// Entries below this level are still stored but hidden by `visible`.
    public var minimumVisibleLevel: LogLevel = .info
    private let capacity: Int

    public init(capacity: Int = 500) {
        self.capacity = capacity
    }

    /// Entries at or above the current threshold, newest last.
    public var visible: [LogEntry] {
        entries.filter { $0.level >= minimumVisibleLevel }
    }

    public mutating func debug(_ m: String) { append(.debug, m) }
    public mutating func info(_ m: String) { append(.info, m) }
    public mutating func warn(_ m: String) { append(.warn, m) }
    public mutating func error(_ m: String) { append(.error, m) }

    private mutating func append(_ level: LogLevel, _ message: String) {
        entries.append(LogEntry(time: Date(), level: level, message: message))
        if entries.count > capacity {
            entries.removeFirst(entries.count - capacity)
        }
    }
}
