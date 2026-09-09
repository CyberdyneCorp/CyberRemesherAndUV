//! Raw FFI bindings to the CyberRemesher C ABI, generated from `cyber_capi.h`.
//!
//! Everything here is `unsafe` and shaped like C. Prefer the `cyberremesh`
//! crate, which owns what the ABI hands back.
//!
//! The bindings are GENERATED, never checked in: a copy in the repository is a
//! copy that can disagree with the header beside it, and the disagreement is
//! silent because our soname does not move between minor releases --
//! `libcyber_capi.so.1` names every 1.x. `build.rs` regenerates them whenever
//! the header, the top-level `CMakeLists.txt` or anything under `cmake/`
//! changes.
//!
//! `bindgen`'s layout assertions are ON (`layout_tests(true)`). Modern bindgen
//! emits them as compile-time `const _: () = { size_of / align_of / offset_of }`
//! rather than `#[test]` functions, so a struct that changed size, alignment or
//! field order fails the BUILD -- no test anyone has to remember to run. That
//! is the closest thing this ABI has to a linker error for a layout change,
//! since the soname deliberately does not move on an additive minor.
//!
//! They check PLACEMENT, not type identity: `const float*` swapped for
//! `const double*` of the same width passes, and so does a field dropped into
//! a struct's existing trailing padding. Both holes are real and both are why
//! the engine's own layout manifest is still open work; this covers the rest of
//! it for free.
#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
