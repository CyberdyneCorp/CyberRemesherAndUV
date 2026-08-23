// Typed Swift error surface over the C ABI's `CyberStatus` codes. Every
// fallible capi call returns a `CyberStatus`; `CyberError.check(_:)` turns a
// non-`CYBER_OK` status into a thrown, richly-typed Swift error, pulling the
// human message from the ABI's thread-local last-error string.
//
// The cases mirror `enum CyberStatus` in capi/include/cyber_capi.h one for one,
// plus `.outOfMemory` for the binding-side "the ABI handed back a NULL handle"
// case, which has no status code of its own.

import CCyberRemesher
import Foundation

/// Errors surfaced by the CyberRemesher engine across the C ABI boundary.
///
/// `.cancelled` is modelled explicitly (rather than as a generic failure) so
/// callers can distinguish a cooperative cancellation from a real error — see
/// ``Mesh/remesh(params:)``.
public enum CyberError: Error, Equatable {
    /// `CYBER_ERR_IO` — file missing, unreadable, or unwritable.
    case io(String)
    /// `CYBER_ERR_INVALID_ARG` — a required pointer argument was NULL, or a
    /// binding-side precondition on a buffer/id failed.
    case invalidArgument(String)
    /// `CYBER_ERR_INVALID_PARAM` — parameters were unusable (NaN, out of range).
    case invalidParameter(String)
    /// `CYBER_ERR_EMPTY` — the mesh had no geometry.
    case empty(String)
    /// `CYBER_ERR_RUNTIME` — the pipeline failed, or the engine was built
    /// without the module the call needs (e.g. `CYBER_BUILD_UV=OFF`).
    case runtime(String)
    /// `CYBER_ERR_CANCELLED` — a long-running call observed cooperative
    /// cancellation and unwound.
    case cancelled
    /// `CYBER_ERR_INCOMPATIBLE_VERSION` — a well-formed versioned file declares
    /// a version this engine does not support.
    case incompatibleVersion(String)
    /// No status code of its own: an allocating ABI call returned a NULL handle.
    case outOfMemory
    /// A status code this binding does not model yet.
    case unknown(code: Int32, message: String)

    /// Throws the mapped error when `status` is not `CYBER_OK`.
    ///
    /// - Parameter status: a raw code returned by any `cyber_*` entry point.
    static func check(_ status: CyberStatus) throws {
        guard status != CYBER_OK else { return }
        throw map(status)
    }

    /// Maps a raw ABI status code to a typed error, attaching the engine's
    /// last-error message where one is available.
    static func map(_ status: CyberStatus) -> CyberError {
        let message = CyberRuntime.lastErrorMessage()
        switch status {
        case CYBER_ERR_IO:
            return .io(message)
        case CYBER_ERR_INVALID_ARG:
            return .invalidArgument(message)
        case CYBER_ERR_INVALID_PARAM:
            return .invalidParameter(message)
        case CYBER_ERR_EMPTY:
            return .empty(message)
        case CYBER_ERR_RUNTIME:
            return .runtime(message)
        case CYBER_ERR_CANCELLED:
            return .cancelled
        case CYBER_ERR_INCOMPATIBLE_VERSION:
            return .incompatibleVersion(message)
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
        case .io(let m): return "I/O failure: \(m)"
        case .invalidArgument(let m): return "Invalid argument: \(m)"
        case .invalidParameter(let m): return "Invalid parameter: \(m)"
        case .empty(let m): return "Mesh is empty: \(m)"
        case .runtime(let m): return "Pipeline failure: \(m)"
        case .cancelled: return "Operation cancelled"
        case .incompatibleVersion(let m): return "Incompatible version: \(m)"
        case .outOfMemory: return "Out of memory"
        case .unknown(let code, let m): return "Engine error \(code): \(m)"
        }
    }
}
