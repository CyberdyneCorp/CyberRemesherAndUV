// UNVERIFIED: requires the Swift toolchain and the `cyber_capi` library; not
// buildable in headless Linux CI. Written against the ABI contract in README.md.
//
// Typed Swift error surface over the C ABI's `CyberStatus` integer codes. Every
// fallible capi call returns a `CyberStatus`; `CyberError.check(_:)` turns a
// non-OK status into a thrown, richly-typed Swift error, pulling the human
// message from the ABI's thread-local last-error string.

import CCyberRemesher

/// Errors surfaced by the CyberRemesher engine across the C ABI boundary.
///
/// Cases mirror the `CYBER_STATUS_*` code family. `.cancelled` is modelled
/// explicitly (rather than as a generic failure) so callers can distinguish a
/// cooperative cancellation from a real error — see `Mesh.remesh(params:)`.
public enum CyberError: Error, Equatable {
    /// A caller passed an invalid argument (null handle, malformed buffer…).
    case invalidArgument(String)
    /// The engine could not allocate memory.
    case outOfMemory
    /// A long-running call observed cooperative cancellation and unwound.
    case cancelled
    /// Mesh import/export or file access failed.
    case io(String)
    /// A parameter was outside its permitted range (e.g. `targetQuads == 0`).
    case outOfRange(String)
    /// The remeshing/parameterization pipeline failed for a specific island.
    case pipeline(String)
    /// A status code the binding does not model yet.
    case unknown(code: Int32, message: String)

    /// Throws the mapped error when `status` is not `CYBER_STATUS_OK`.
    ///
    /// - Parameter status: a raw code returned by any `cyber_*` entry point.
    static func check(_ status: CyberStatus) throws {
        guard status != CYBER_STATUS_OK else { return }
        throw map(status)
    }

    /// Maps a raw ABI status code to a typed error, attaching the engine's
    /// last-error message where one is available.
    static func map(_ status: CyberStatus) -> CyberError {
        let message = CyberRuntime.lastErrorMessage()
        switch status {
        case CYBER_STATUS_INVALID_ARGUMENT:
            return .invalidArgument(message)
        case CYBER_STATUS_OUT_OF_MEMORY:
            return .outOfMemory
        case CYBER_STATUS_CANCELLED:
            return .cancelled
        case CYBER_STATUS_IO:
            return .io(message)
        case CYBER_STATUS_OUT_OF_RANGE:
            return .outOfRange(message)
        case CYBER_STATUS_PIPELINE:
            return .pipeline(message)
        default:
            // The C enum's rawValue may import as UInt32 or Int32 depending on
            // the platform's underlying enum type; normalize to Int32.
            return .unknown(code: Int32(truncatingIfNeeded: status.rawValue), message: message)
        }
    }
}

extension CyberError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .invalidArgument(let m): return "Invalid argument: \(m)"
        case .outOfMemory: return "Out of memory"
        case .cancelled: return "Operation cancelled"
        case .io(let m): return "I/O failure: \(m)"
        case .outOfRange(let m): return "Parameter out of range: \(m)"
        case .pipeline(let m): return "Pipeline failure: \(m)"
        case .unknown(let code, let m): return "Engine error \(code): \(m)"
        }
    }
}
