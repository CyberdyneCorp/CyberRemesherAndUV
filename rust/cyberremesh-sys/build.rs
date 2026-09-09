//! Finds or builds the engine, links it, and generates the raw bindings.
//!
//! Written with ClaySpaceDesktop's `cyberremesh-sys` as the reference
//! implementation -- the first external embedder of this engine, which paid for
//! several of the lessons encoded below before we had a Rust crate of our own.
//! Where this file diverges from theirs it is because a FIRST-PARTY crate has
//! different pressures than a single-consumer vendored one, and each divergence
//! says so at its site.
//!
//! Two discovery paths, chosen EXPLICITLY rather than inferred:
//!
//!   CYBERREMESH_LIB_DIR   link a prebuilt library from this directory
//!   (unset)               configure and build the engine in this repository
//!
//! A named directory that does not contain the library is a hard error, never
//! a quiet fall back to building: "the prebuilt tree I named was ignored" is
//! the kind of failure that surfaces as linking something other than what you
//! meant, which is the failure mode with no legible symptom.

use std::path::{Path, PathBuf};
use std::process::Command;

const REQUIRED_CMAKE: (u32, u32) = (3, 24);

fn main() {
    // This crate lives IN the engine's repository, so the source is two levels
    // up rather than in a vendored submodule. A consumer vendoring us gets the
    // whole repo and this still resolves.
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let engine = manifest
        .join("../..")
        .canonicalize()
        .expect("the engine source should be two levels above this crate");

    assert_header(&engine);
    emit_rerun_directives(&engine);

    let link_dir = match std::env::var_os("CYBERREMESH_LIB_DIR") {
        Some(dir) => use_prebuilt(Path::new(&dir)),
        None => {
            check_cmake();
            build_engine(&engine)
        }
    };
    emit_link_flags(&link_dir, std::env::var_os("CYBERREMESH_LIB_DIR").is_none());
    generate_bindings(&engine);
}

fn assert_header(engine: &Path) {
    let header = engine.join("capi/include/cyber_capi.h");
    assert!(
        header.is_file(),
        "\n\ncyber_capi.h is missing at {}\n\nThis crate builds the engine it \
         ships with; if you are vendoring this repository, vendor it whole.\n",
        header.display()
    );
}

/// A prebuilt tree, named by the caller.
///
/// Distribution packagers need this: they build the engine once, with their own
/// flags and dependency versions, and a crate that insists on compiling its own
/// copy is unpackageable. Contributors want the other path.
fn use_prebuilt(dir: &Path) -> PathBuf {
    let found = ["libcyber_capi.so", "libcyber_capi.dylib", "cyber_capi.lib"]
        .iter()
        .any(|name| dir.join(name).exists());
    assert!(
        found,
        "\n\nCYBERREMESH_LIB_DIR is set to {} but no cyber_capi library is \
         there.\n\nThis is a hard error rather than a fall back to building, \
         deliberately: silently building a second copy would link something \
         other than the library you named, and nothing would say so.\n",
        dir.display()
    );
    println!(
        "cargo:warning=cyberremesh-sys: linking the prebuilt library in {} \
         rather than building the engine. Its build options -- above all \
         whether it carries the in-process QuadCover solver -- are yours to \
         guarantee; call cyber_seamless_solver() to see what you got.",
        dir.display()
    );
    dir.to_path_buf()
}

fn check_cmake() {
    let Some(version) = Command::new("cmake")
        .arg("--version")
        .output()
        .ok()
        .and_then(|out| String::from_utf8(out.stdout).ok())
    else {
        panic!("\n\nCMake was not found, and this crate builds the engine with it.\n");
    };
    let numbers: Vec<u32> = version
        .lines()
        .next()
        .unwrap_or_default()
        .split_whitespace()
        .find(|word| word.chars().next().is_some_and(|c| c.is_ascii_digit()))
        .map(|v| v.split('.').filter_map(|p| p.parse().ok()).collect())
        .unwrap_or_default();
    if let (Some(&major), Some(&minor)) = (numbers.first(), numbers.get(1)) {
        assert!(
            (major, minor) >= REQUIRED_CMAKE,
            "\n\nCMake {major}.{minor} is older than the {}.{} this engine requires.\n",
            REQUIRED_CMAKE.0,
            REQUIRED_CMAKE.1
        );
    }
}

fn build_engine(engine: &Path) -> PathBuf {
    let mut cfg = cmake::Config::new(engine);
    // EVERY NAME HERE IS READ OUT OF THE ENGINE'S OWN CMakeLists.txt, never
    // guessed from a sibling project's pattern. A `-D` for an option a project
    // does not define is SILENTLY IGNORED by CMake, so a wrong name is not an
    // error -- it is a setting that quietly does not apply, and the build looks
    // like it honoured you. The reference implementation lost a debugging cycle
    // to exactly this (it guessed `CYBER_BACKEND_*`; they are `CYBER_ENABLE_*`).
    cfg.define("CYBER_BUILD_TESTS", "OFF")
        .define("CYBER_BUILD_CLI", "OFF")
        .define("CYBER_BUILD_NET", "OFF")
        .define("CYBER_BUILD_RENDER", "OFF")
        .define("CYBER_BUILD_APPS", "OFF")
        // The C ABI facade and the modules behind it.
        .define("CYBER_BUILD_CAPI", "ON")
        .define("CYBER_BUILD_UV", "ON")
        .define("CYBER_BUILD_RETOPO", "ON")
        .define("CYBER_BUILD_BAKECAGE", "ON")
        // The in-process QuadCover solver. Enabled is NOT the same as required:
        // without it the engine does not fail, it routes to the portable
        // quadrangulator and returns genuinely different quads. See the
        // `require-quadcover` feature for turning that into a configure error,
        // and cyber_seamless_solver() for asking at runtime what you got.
        .define("CYBER_WITH_QUADCOVER", "ON")
        // No GPU backend. All three already default OFF; stated so a future
        // default flip cannot quietly put this engine on a device a host is
        // already using for something else.
        .define("CYBER_ENABLE_CUDA", "OFF")
        .define("CYBER_ENABLE_METAL", "OFF")
        .define("CYBER_ENABLE_OPENCL", "OFF")
        .define("CMAKE_BUILD_TYPE", "Release")
        .define("CMAKE_POSITION_INDEPENDENT_CODE", "ON")
        // The SHARED target, not the static one. `cyber_capi` is a static
        // archive holding only capi.cpp and declaring its dependencies PUBLIC,
        // which propagates through CMake's link interface ONLY -- hand-linking
        // it from here gets capi.cpp and nothing it calls. `cyber_capi_shared`
        // links every module privately and is what the engine documents for
        // consumers.
        //
        // It is also the target carrying the linker version script, which
        // matters more than its size: it exports only `cyber_*`, so the
        // vendored Geogram, stb and tinygltf definitions inside cannot be
        // interposed by a host that carries its own copies of the same
        // third-party code, or interpose on them. Two C++ engines sharing a
        // symbol namespace is a class of bug nobody wants to debug.
        .build_target("cyber_capi_shared");

    if cfg!(feature = "require-quadcover") {
        cfg.define("CYBER_REQUIRE_QUADCOVER", "ON");
    }

    let out = cfg.build();
    announce_the_solver_this_build_got(&out.join("build"));
    out.join("build/capi")
}

/// Says, out loud, when this build did NOT get the in-process solver.
///
/// The feature above defaults OFF because a crate that fails to configure on a
/// fresh macOS or Windows checkout is judged broken rather than strict. But
/// defaulting it off makes the QUIET build the default build, and quiet is the
/// entire problem: what makes the fallback to the portable quadrangulator
/// dangerous is not the fallback, it is that nobody knows it happened. Turning
/// the failure off without turning the announcement on would have shipped that
/// to every consumer instead of one platform.
///
/// This reports what the build GOT rather than what the platform suggests it
/// should have. `cyber_quadcover_solver` is a discrete target that CMake builds
/// only when it finds OpenMP and TBB (`cmake/QuadCoverSolver.cmake`), so the
/// artifact's existence is ground truth — and a platform conditional would be a
/// guess that is wrong on any Linux box missing TBB.
fn announce_the_solver_this_build_got(build: &Path) {
    let solver = build.join("src/quadrangulate");
    let found = ["libcyber_quadcover_solver.a", "cyber_quadcover_solver.lib"]
        .iter()
        .any(|name| solver.join(name).exists());
    if found {
        return;
    }
    println!(
        "cargo:warning=cyberremesh-sys: built WITHOUT the in-process QuadCover          solver (OpenMP and TBB were not found). The engine does not fail for          this -- it routes to the portable quadrangulator and produces          GENUINELY DIFFERENT QUADS. Enable the `require-quadcover` feature to          make this a configure error instead, and call          cyberremesh::solver() to confirm at runtime what you linked."
    );
}

fn emit_link_flags(dir: &Path, built_here: bool) {
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=cyber_capi");
    // An rpath into our own build directory, and ONLY when we built it. It is
    // what makes `cargo test` work without an LD_LIBRARY_PATH incantation. For
    // a prebuilt tree it would bake a path from THIS machine into the binary,
    // which is the packager's decision and not ours to make for them.
    if built_here && !cfg!(target_os = "windows") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
    }
    println!("cargo:lib_dir={}", dir.display());
}

fn generate_bindings(engine: &Path) {
    let include = engine.join("capi/include");
    let bindings = bindgen::Builder::default()
        .header(include.join("cyber_capi.h").to_string_lossy())
        .clang_arg(format!("-I{}", include.display()))
        // All THREE allowlists, and the var one is load-bearing rather than
        // decorative: CYBER_QUAD_*, CYBER_SUBDIV_*, CYBER_ABI_VERSION_* and the
        // rest are preprocessor constants, so without allowlist_var they are
        // simply absent and a binding author hand-mirrors five integers that
        // then drift. With it they come from the header and cannot.
        .allowlist_function("cyber_.*")
        .allowlist_type("Cyber.*")
        .allowlist_var("CYBER_.*")
        // A module of consts per enum rather than a Rust enum. The C ABI may
        // add a status code, and a `rustified_enum` turns that into undefined
        // behaviour on the client the day it happens -- there is no exhaustive
        // match to break here, which is the point.
        .default_enum_style(bindgen::EnumVariation::ModuleConsts)
        .derive_debug(true)
        // Params structs are filled by cyber_default_*_params and then
        // overwritten field by field. Deriving Default means a field ADDED
        // upstream is zero rather than uninitialised until the safe layer
        // learns about it.
        .derive_default(true)
        // ON, and the reference implementation had it off. See the crate docs:
        // modern bindgen emits these as compile-time assertions, so a struct
        // whose size, alignment or field order moved fails the BUILD. Our
        // soname deliberately does not move on an additive minor, so this is
        // the closest thing to a linker error a layout change can get.
        .layout_tests(true)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("cyber_capi.h could not be bound");
    let out = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    bindings
        .write_to_file(out.join("bindings.rs"))
        .expect("the generated bindings could not be written");
}

fn emit_rerun_directives(engine: &Path) {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=CYBERREMESH_LIB_DIR");
    println!(
        "cargo:rerun-if-changed={}",
        engine.join("capi/include/cyber_capi.h").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        engine.join("CMakeLists.txt").display()
    );
    // Whole DIRECTORIES, not just the header and the top-level list file.
    //
    // `cmake/` because the option deciding whether this build gets the
    // in-process solver at all lives in cmake/QuadCoverSolver.cmake, not in
    // CMakeLists.txt.
    //
    // `capi/` and `src/` because without them, editing the engine's own C++
    // leaves Cargo believing this script's output is current: CMake never
    // re-runs, and the previously built library stays linked while the headers
    // beside it have moved.
    //
    // Watching whole directories rather than a list of files that looks
    // complete is the point, and the severity argues for it here more than it
    // did where this was found. The reference implementation is a CONSUMER with
    // the engine vendored, so editing the engine's `src/` is rare and
    // deliberate and the hole is a footnote. This crate lives INSIDE the engine
    // repository, where editing `src/` and rebuilding is the most common thing
    // a contributor does -- the same omission is a daily wrong build.
    for dir in ["capi", "src", "cmake"] {
        println!("cargo:rerun-if-changed={}", engine.join(dir).display());
    }
}
