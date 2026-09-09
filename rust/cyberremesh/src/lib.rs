//! Quad remeshing, retopology, UV and baking — the CyberRemesher engine, safely.
//!
//! Everything above this line reaches the engine through owned types that free
//! what the C ABI hands back, so a refused call cannot leak.
//!
//! # Check the ABI at startup
//!
//! ```no_run
//! cyberremesh::check_abi().expect("the linked engine cannot serve this crate");
//! ```
//!
//! Cheap, and the failure it catches is otherwise silent: the engine's soname
//! is `libcyber_capi.so.1` for every 1.x release, so a library from a different
//! minor loads without complaint and fails later as behaviour.
//!
//! # Two rules that are not negotiable
//!
//! **A mesh handle's element ids move.** Most retopology operations reassign
//! them and subdivision reassigns *all* of them. Anything you key on a vertex
//! or face id — a selection, a pin, a mapping back into your own scene — is
//! stale the moment such a call returns. Either build, use and drop per
//! operation, or record [`Mesh::topology_generation`] beside that state and
//! compare before trusting it. Equal values prove your ids are good; differing
//! values only mean you must not assume.
//!
//! **Cap the worker pool before any work runs, if you are interactive.**
//! Uncapped, the engine sizes its parallel loops from hardware concurrency,
//! which is right for a batch run and wrong inside an application trying to
//! share those cores. The cap cannot change a result — the engine pins that
//! with a test comparing a capped run against an uncapped one — so it costs a
//! result nothing and costs a stroke everything.
//!
//! # Which solver you got
//!
//! A build without the in-process QuadCover field does **not** fail: it routes
//! to the portable quadrangulator and returns genuinely different quads. The
//! `require-quadcover` feature makes that a configure error, and [`solver`]
//! answers at runtime. They catch different failures — the feature cannot see a
//! *different* library being loaded, since the soname names every 1.x — so
//! neither alone is the guard.
//!
//! # Credit
//!
//! The build script is written with ClaySpaceDesktop's `cyberremesh-sys` as its
//! reference implementation. It was the first external embedder of this engine
//! and paid for several of the lessons encoded there — the CMake option names
//! that are silently ignored when wrong, the shared-versus-static target, and
//! the platform conditional around requiring QuadCover — before we had a Rust
//! crate of our own.
#![forbid(clippy::undocumented_unsafe_blocks)]

use cyberremesh_sys as sys;

mod error;
mod mesh;
mod version;

pub use error::{Error, Result};
pub use mesh::Mesh;
pub use version::{abi, check_abi, solver, version, Solver, Version, ABI_MAJOR, ABI_MINOR};

/// Caps the engine's worker pool. Zero means uncapped, the engine's default.
///
/// Callable at any time from any thread; loops already running keep the
/// fan-out they started with, so this is a decision for startup rather than
/// something to modulate mid-operation.
pub fn set_max_worker_threads(threads: i32) -> Result<()> {
    // SAFETY: a plain integer setter with no handle and no out-parameter. The
    // header documents it as callable from any thread at any time.
    //
    // The status is checked rather than discarded: a refused cap leaves the
    // pool UNCAPPED, which is the state this call exists to prevent, so
    // ignoring the return would silently do the opposite of what was asked.
    let status = unsafe { sys::cyber_set_max_worker_threads(threads) };
    error::check(status, "cyber_set_max_worker_threads")
}

/// What the pool is capped to, or zero for uncapped.
pub fn max_worker_threads() -> i32 {
    // SAFETY: reads a process-global; no handle, cannot fail.
    unsafe { sys::cyber_max_worker_threads() }
}

/// Refuses to import a mesh file carrying more than `max_vertices`. Zero is off.
///
/// A RESOURCE bound, not a security one. Malformed input is already refused
/// structurally — a declared element count is checked against the bytes the
/// file actually carries — and needs no number from you. This bounds a
/// LEGITIMATE file too large for your budget.
///
/// Off by default because the engine cannot know your budget. Pick a number
/// your target device can afford: a ceiling that is never reached is not a
/// ceiling.
pub fn set_max_import_vertices(max_vertices: u64) -> Result<()> {
    // SAFETY: a plain integer setter with no handle and no out-parameter.
    let status = unsafe { sys::cyber_set_max_import_vertices(max_vertices) };
    error::check(status, "cyber_set_max_import_vertices")
}

/// The current import ceiling, or zero for none.
pub fn max_import_vertices() -> u64 {
    // SAFETY: reads a process-global; no handle, cannot fail.
    unsafe { sys::cyber_max_import_vertices() }
}
