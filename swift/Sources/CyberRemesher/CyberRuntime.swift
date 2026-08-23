// Runtime metadata helpers: the engine's semantic version, the static status
// strings, and access to the thread-local last-error message used by
// `CyberError`.
//
// The C ABI carries NO separate "ABI version" symbol — `cyber_version` reports
// the engine's semantic version (mirroring the CMake project version) and that
// is the only compatibility signal available. Compatibility gating for the
// versioned data formats lives with those formats: the sculpt-handoff bridge
// declares `CYBER_HANDOFF_VERSION_*` and fails an unsupported file with
// `CYBER_ERR_INCOMPATIBLE_VERSION`.

import CCyberRemesher

/// Static entry points that describe the loaded engine build.
public enum CyberRuntime {
    /// Semantic version of the linked engine, as `(major, minor, patch)`.
    public static var versionComponents: (major: Int, minor: Int, patch: Int) {
        var major: Int32 = 0
        var minor: Int32 = 0
        var patch: Int32 = 0
        cyber_version(&major, &minor, &patch)
        return (Int(major), Int(minor), Int(patch))
    }

    /// Human-readable semantic version of the linked engine (e.g. `"0.5.0"`).
    public static var version: String {
        let components = versionComponents
        return "\(components.major).\(components.minor).\(components.patch)"
    }

    /// Version of the sculpt-handoff interchange format this build writes and
    /// can read. A handoff file declaring a higher major fails to open with
    /// ``CyberError/incompatibleVersion(_:)``.
    public static let handoffFormatVersion = (
        major: Int(CYBER_HANDOFF_VERSION_MAJOR),
        minor: Int(CYBER_HANDOFF_VERSION_MINOR)
    )

    /// The engine's static, human-readable description of a status code.
    static func description(of status: CyberStatus) -> String {
        guard let raw = cyber_status_string(status) else { return "" }
        return String(cString: raw)
    }

    /// The engine's thread-local last-error message, or an empty string when
    /// the previous call on this thread succeeded.
    static func lastErrorMessage() -> String {
        guard let raw = cyber_last_error() else { return "" }
        return String(cString: raw)
    }
}
