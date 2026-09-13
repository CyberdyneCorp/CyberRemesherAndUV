// Runtime metadata helpers: the engine's semantic version, the static status
// strings, and access to the thread-local last-error message used by
// `CyberError`.
//
// TWO version numbers, and they answer different questions. `cyber_abi_version`
// reports the ABI — the shape of the C surface, which decides whether compiled
// calls link and mean what they were compiled against. `cyber_version` reports
// the ENGINE, the behaviour behind that shape; a matching ABI does not promise
// the same mesh. Compatibility gating for the versioned data formats lives with
// those formats: the sculpt-handoff bridge declares `CYBER_HANDOFF_VERSION_*`
// and fails an unsupported file with `CYBER_ERR_INCOMPATIBLE_VERSION`.

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

    /// C ABI version of the linked library, as `(major, minor)`.
    ///
    /// Distinct from ``version``: this describes the SURFACE, not the
    /// behaviour. Use ``checkABI()`` rather than comparing these by hand — the
    /// compatibility rule lives in the engine so every binding agrees.
    public static var abiVersionComponents: (major: Int, minor: Int) {
        var major: Int32 = 0
        var minor: Int32 = 0
        cyber_abi_version(&major, &minor)
        return (Int(major), Int(minor))
    }

    /// Which seamless-UV solver the linked build carries.
    ///
    /// `"native+geogram"` when the in-process Geogram QuadCover solver is
    /// compiled in, `"native"` when it is not. Worth asserting at startup: a
    /// build without it does not fail, it routes to the portable quadrangulator
    /// and returns genuinely different quads, and nothing else says so.
    public static var seamlessSolver: String {
        String(cString: cyber_seamless_solver())
    }

    /// The ABI this Swift package was written against.
    public static let abiVersionCompiledAgainst = (major: 1, minor: 7)

    /// Throw if the loaded library cannot serve this package's compiled ABI.
    public static func checkABI() throws {
        let compiled = abiVersionCompiledAgainst
        try CyberError.check(
            cyber_abi_check(Int32(compiled.major), Int32(compiled.minor)))
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
