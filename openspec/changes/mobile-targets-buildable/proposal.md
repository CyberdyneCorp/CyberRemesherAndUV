# Proposal: make the mobile targets provably buildable, testable and bounded

## Why

The product is an iPad app. Three of the things that make it one were never
verified by anything:

- **The `android` preset had never configured.** `src/accel/CMakeLists.txt`
  asked for `find_package(OpenCL REQUIRED)` whenever `CYBER_ENABLE_OPENCL` was
  on, the preset turned it on, and the NDK ships no OpenCL — so the configure
  aborted before any other setting in that preset was ever reached. Both mobile
  presets also claimed backends in their `displayName` that they did not
  enable, and built the POSIX-socket network bridge an app has no use for.
- **`apps/cli/main.cpp` could not compile for either mobile target.** Its
  number parser instantiated `std::from_chars` for `float`, and libc++ — the
  standard library of both Apple platforms and the Android NDK — declares the
  floating-point overloads deleted.
- **No lane ran the engine on 64-bit ARM.** Every CI leg is x86-64, which
  cannot see a defect that depends on the mobile word size, alignment, the
  signedness of `char`, or floating-point contraction (aarch64 fuses `a*b+c`
  into one FMA where baseline x86-64 cannot).

Two more gaps sat next to them. `src/accel/src/metal_backend.mm` was added
**without** `enable_language(OBJCXX)`, so CMake fed it to the plain C++ driver
and it never got ARC; and it is the one file in the repository no lane compiles
at all, since the only Apple job runs `continue-on-error`. And a host had no
way to bound the engine's worker threads: both parallel loops read
`std::thread::hardware_concurrency()` directly, so a bake took every core of a
tablet the app was trying to share with the OS.

## What Changes

- The `android` preset leaves OpenCL off (with the `REQUIRED` replaced by a
  diagnostic that names the NDK), both mobile presets turn the network bridge
  off, and their `displayName`s describe what they actually enable.
- The CLI's number parser keeps `std::from_chars` where it exists and reimposes
  its exact grammar over `strtof`/`strtod` where it does not.
- A **cross-compiled arm64 lane** builds the whole library, CLI and suite for
  aarch64 and runs the suite under `qemu-user`.
- `cyber_accel_metal` declares OBJCXX and compiles with `-fobjc-arc`, and an
  **off-Apple Objective-C++ syntax gate** compiles `metal_backend.mm` against
  declaration-only Metal/Foundation stubs on every platform, mutation-checked
  so it cannot pass vacuously.
- A documented **worker-thread cap** (`cyber::setMaxWorkerThreads`, C ABI
  `cyber_set_max_worker_threads`) that both parallel loops honour, and that
  cannot change a single output byte.

## Capabilities

### Modified Capabilities

- `build-and-packaging`: mobile presets must configure; arm64 must be executed
  in CI; the Objective-C++ source must be compiled by some lane on every host.
- `compute-acceleration`: the host gets bounded control of the worker fan-out.
- `engine-bindings`: the C ABI exposes that control.

## Impact

- `CMakePresets.json`, `cmake/toolchains/aarch64-linux-gnu.cmake`,
  `src/accel/CMakeLists.txt`, `src/core/CMakeLists.txt`
- `apps/cli/main.cpp`, `apps/cli/parse_number.hpp`
- `src/core/include/cyber/core/threading.hpp`, `src/core/src/threading.cpp`,
  `src/core/src/pipeline.cpp`, `src/accel/src/backend_primitives.cpp`,
  `src/accel/src/cpu_backend.cpp`
- `capi/include/cyber_capi.h`, `capi/src/capi.cpp`
- `.github/workflows/ci.yml`, `tests/CMakeLists.txt`,
  `tests/packaging/test_metal_syntax.py`, `tests/packaging/objcxx_stubs/`
- Output is byte-identical: no mesh, bake or atlas result changes.
