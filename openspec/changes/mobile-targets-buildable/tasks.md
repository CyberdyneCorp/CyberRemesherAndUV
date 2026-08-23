# Tasks

## 1. Presets that configure

- [x] 1.1 Turn OpenCL off in the `android` preset; replace `find_package(OpenCL REQUIRED)` with a diagnostic naming the NDK
- [x] 1.2 Turn `CYBER_BUILD_NET` off on both mobile presets (shared `mobile-base`)
- [x] 1.3 Correct both mobile `displayName`s to what the presets actually enable
- [x] 1.4 Verified: `android` preset configures and builds (library + CLI + suite) against NDK r26d, arm64-v8a, 148 steps, zero warnings

## 2. The CLI compiles for mobile

- [x] 2.1 Extract the number parser to `apps/cli/parse_number.hpp` with a strtof/strtod path for libc++
- [x] 2.2 Regression test `tests/cli/test_parse_number.cpp` holds the fallback to `std::from_chars`' exact verdicts
- [x] 2.3 Verified against real libc++ 18 and against the NDK's libc++ (clang 17)

## 3. arm64 is executed, not just compiled

- [x] 3.1 `cmake/toolchains/aarch64-linux-gnu.cmake` + `linux-arm64-cross` preset (configure/build/test)
- [x] 3.2 Hand the cross emulator to the Python-driven cases (`CYBER_TEST_LAUNCHER`); skip the benchmark on a cross build
- [x] 3.3 `arm64-cross` CI lane
- [x] 3.4 Verified locally: 146 build steps clean, suite runs under qemu-aarch64

## 4. The Objective-C++ file stops being unguarded

- [x] 4.1 `enable_language(OBJCXX)` + `-fobjc-arc` for `cyber_accel_metal`
- [x] 4.2 Declaration-only Metal/Foundation stubs (`tests/packaging/objcxx_stubs/`)
- [x] 4.3 `tests/packaging/test_metal_syntax.py`, registered as ctest `metal_objcxx_syntax`, self-mutation-checked
- [x] 4.4 `ios-preset` CI lane (reporting; only Apple hardware can promote it)

## 5. Host control over the worker fan-out

- [x] 5.1 `cyber::setMaxWorkerThreads` / `maxWorkerThreads` / `workerThreadsFor` in core
- [x] 5.2 Both call sites honour it (pipeline relax loops, accel `parallelFor`); the CPU device name reports the effective count
- [x] 5.3 C ABI `cyber_set_max_worker_threads` / `cyber_max_worker_threads`
- [x] 5.4 Regression tests: `tests/core/test_threading.cpp` (structural chunk counts + capped-vs-uncapped output identity), `tests/capi/test_capi.cpp`

## 6. Output identity

- [x] 6.1 77 mesh/atlas/bundle outputs over 6 real models, byte-identical before and after
