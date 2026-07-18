// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the ABI contract in README.md.
//
// Runtime metadata helpers: semantic version string, numeric ABI version for
// compatibility gating, and access to the engine's thread-local last-error
// message used by CyberError.

import CCyberRemesher

/// Static entry points that describe the loaded engine build.
public enum CyberRuntime {
    /// Human-readable semantic version of the linked engine (e.g. `"1.0.0"`).
    public static var version: String {
        guard let raw = cyber_version() else { return "unknown" }
        return String(cString: raw)
    }

    /// Numeric ABI version. Bindings should refuse to operate against an engine
    /// whose ABI major differs from the one they were built against.
    public static var abiVersion: UInt32 {
        cyber_abi_version()
    }

    /// The ABI major version this Swift package was authored against.
    public static let supportedAbiMajor: UInt32 = 1

    /// Verifies the linked engine speaks a compatible ABI.
    ///
    /// - Throws: `CyberError.unknown` if the engine's ABI major differs.
    public static func verifyCompatibility() throws {
        let major = abiVersion >> 16
        guard major == supportedAbiMajor else {
            throw CyberError.unknown(
                code: -1,
                message: "Incompatible ABI major \(major); expected \(supportedAbiMajor)"
            )
        }
    }

    /// The engine's thread-local last-error message, or an empty string.
    static func lastErrorMessage() -> String {
        guard let raw = cyber_last_error_message() else { return "" }
        return String(cString: raw)
    }
}
