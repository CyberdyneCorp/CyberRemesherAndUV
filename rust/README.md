# Rust bindings

Two crates, mirroring the layout of `python/` and `swift/`:

| crate | what |
|---|---|
| `cyberremesh-sys` | raw FFI, generated from `capi/include/cyber_capi.h` |
| `cyberremesh` | safe wrapper — owned handles, `Result`, no `unsafe` above it |

```rust
cyberremesh::check_abi()?;               // do this first
let mesh = cyberremesh::Mesh::load("in.obj")?;
println!("{} verts, {} faces", mesh.vertex_count(), mesh.face_count());
```

## Building

`cargo build` compiles the engine through CMake as a side effect, so the first
build is slow and needs CMake ≥ 3.24 and a C++20 toolchain. For the in-process
QuadCover solver it also wants OpenMP and TBB (`libtbb-dev` on Debian).

To link a library you built yourself instead:

```sh
CYBERREMESH_LIB_DIR=/path/to/build/capi cargo build
```

A directory that does not contain the library is a **hard error**, never a
quiet fall back to building — linking something other than what you named has
no legible symptom.

## Two things worth wiring up

**Check the ABI at startup.** `libcyber_capi.so.1` names *every* 1.x release, so
a library from a different minor loads without complaint and fails later as
behaviour. `check_abi()` compares what this crate compiled against with what
loaded, using the rule implemented inside the engine so every binding agrees.

**Know which solver you got.** A build without the in-process QuadCover field
does not fail — it routes to the portable quadrangulator and returns genuinely
different quads. Two halves catch different failures:

- the `require-quadcover` feature makes a missing dependency a *configure*
  error. Off by default, because a crate that fails to configure on a fresh
  macOS checkout is judged broken rather than strict.
- `cyberremesh::solver()` answers at *runtime*, which is the only half that can
  see a different `libcyber_capi.so.1` having been loaded.

When the feature is off and the build did not get the solver, the build script
says so as a `cargo:warning` — what makes that fallback dangerous is not the
fallback, it is nobody knowing it happened.

## Scope

The `-sys` crate binds the whole ABI (192 entry points). The safe wrapper
currently covers version/ABI, errors, and mesh load/save/inspect. UV, baking,
remeshing and conform are reachable through `Mesh::as_raw` until wrapped.

## Credit

The build script is written with
[ClaySpaceDesktop](https://github.com/CyberdyneCorp)'s `cyberremesh-sys` as its
reference implementation. It was the first external embedder of this engine and
paid for several of the lessons encoded here before we had a Rust crate of our
own — that a `-D` for an option CMake does not define is silently ignored, that
`cyber_capi_shared` rather than the static archive is the consumer's target, and
that requiring QuadCover on a platform whose CI cannot supply it only breaks the
build.
