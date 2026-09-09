//! Which engine this is, which ABI it implements, and which solver it got.
//!
//! THREE questions, and conflating any two of them is how a host ends up
//! debugging output quality when the fault was a build option.

use crate::error::{check, Result};
use cyberremesh_sys as sys;

/// The C ABI this crate was compiled against, taken from the header rather
/// than written here.
///
/// `allowlist_var("CYBER_.*")` in the -sys build script is what makes these
/// reachable; without it a binding author hand-mirrors the numbers and they
/// drift silently from the header beside them.
pub const ABI_MAJOR: i32 = sys::CYBER_ABI_VERSION_MAJOR as i32;
pub const ABI_MINOR: i32 = sys::CYBER_ABI_VERSION_MINOR as i32;

/// The engine's project version — its BEHAVIOUR, not its shape.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
}

impl std::fmt::Display for Version {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

/// The engine's project version.
///
/// A MATCHING ABI DOES NOT PROMISE THE SAME MESH. This is the number that moves
/// when the quads change; [`abi`] is the number that decides whether the calls
/// link and mean what you compiled against. Pin this (or a commit) for
/// reproducible output.
pub fn version() -> Version {
    let (mut major, mut minor, mut patch) = (0, 0, 0);
    // SAFETY: three valid out-pointers; the call writes each and reads nothing.
    unsafe { sys::cyber_version(&mut major, &mut minor, &mut patch) };
    Version {
        major: major as u32,
        minor: minor as u32,
        patch: patch as u32,
    }
}

/// The C ABI the loaded library implements, as `(major, minor)`.
pub fn abi() -> (i32, i32) {
    let (mut major, mut minor) = (0, 0);
    // SAFETY: two valid out-pointers; cannot fail, either may be null.
    unsafe { sys::cyber_abi_version(&mut major, &mut minor) };
    (major, minor)
}

/// Fails unless the loaded library can serve a client compiled against this
/// crate's ABI.
///
/// The rule — same major, library minor at least ours — lives in the ENGINE
/// (`cyber_abi_check`) and not here, so a Rust host, a Python host and a C host
/// all get the same verdict rather than three reimplementations of a comparison
/// that is easy to get backwards.
///
/// Call it at startup. It is cheap, and the failure it catches is otherwise
/// silent: our soname is `libcyber_capi.so.1` for every 1.x release, so a
/// library from a different minor loads without complaint.
pub fn check_abi() -> Result<()> {
    // SAFETY: two plain integers, no handle, no out-parameter.
    let status = unsafe { sys::cyber_abi_check(ABI_MAJOR, ABI_MINOR) };
    check(status, "cyber_abi_check")
}

/// Which seamless-UV solver the loaded library was built with.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Solver {
    /// The in-process Geogram QuadCover field: the shipping default.
    NativeAndGeogram,
    /// The portable quadrangulator. Produces genuinely different quads.
    Native,
    /// A name this crate does not know. CARRIED rather than mapped to a
    /// default, because the set may grow and guessing which side of the
    /// difference an unknown name falls on is worse than saying so.
    Other(String),
}

/// Which solver the loaded library carries.
///
/// Worth asserting at startup if your build is supposed to have the in-process
/// field: a build without it does NOT fail, it routes to the portable
/// quadrangulator and returns different quads with nothing to announce it.
///
/// This is the LOAD-TIME half of a pair. The `require-quadcover` feature is the
/// build-time half, turning a missing dependency into a configure error — and
/// it cannot see a *different* `libcyber_capi.so.1` being loaded later, because
/// the soname names every 1.x release. Neither alone is the guard.
pub fn solver() -> Solver {
    // SAFETY: returns a static NUL-terminated string, never null.
    let raw = unsafe { sys::cyber_seamless_solver() };
    if raw.is_null() {
        return Solver::Other(String::new());
    }
    // SAFETY: non-null and NUL-terminated, owned by the engine and static.
    let name = unsafe { std::ffi::CStr::from_ptr(raw) }.to_string_lossy();
    match name.as_ref() {
        "native+geogram" => Solver::NativeAndGeogram,
        "native" => Solver::Native,
        other => Solver::Other(other.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_loaded_library_serves_this_crate() {
        check_abi().expect("the linked engine cannot serve this crate's ABI");
        let (major, minor) = abi();
        assert_eq!(major, ABI_MAJOR);
        assert!(
            minor >= ABI_MINOR,
            "library ABI {major}.{minor} is older than the {ABI_MAJOR}.{ABI_MINOR} \
             this crate compiled against"
        );
    }

    #[test]
    fn the_abi_and_the_engine_version_are_different_numbers() {
        // The whole reason there are two. If a future change ever derives one
        // from the other -- a design the engine explicitly rejected, because
        // its project version moves for quality work that changes no
        // declaration -- this is what notices.
        let (abi_major, _) = abi();
        assert_ne!(
            abi_major,
            version().major as i32,
            "the ABI major and the engine major have collapsed into one number"
        );
    }

    #[test]
    fn the_solver_is_one_this_crate_knows() {
        // Not asserting WHICH: that depends on build options and on whether the
        // host set `require-quadcover`. Asserting only that the name parses, so
        // an unrecognised one shows up here rather than as quads nobody can
        // explain.
        assert!(
            matches!(solver(), Solver::NativeAndGeogram | Solver::Native),
            "unrecognised seamless solver: {:?}",
            solver()
        );
    }
}
