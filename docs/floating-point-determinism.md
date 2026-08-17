# Floating-point determinism across architectures

The engine's strongest correctness gates compare **raw IEEE-754 bits**: the bake
pixel checksums in `tests/bake/test_field_bake.cpp`, the mesh goldens, the
preset/bake bundles. A gate like that only holds while every platform rounds the
arithmetic the same way. This is what makes that true, and what still does not.

## The rule: no fused multiply-add contraction

`cmake/FloatingPoint.cmake` puts `-ffp-contract=off` on every translation unit
in the tree — project code, vendored thirdparty and the vendored QuadCover solver
alike, because a value that crosses a module boundary is only reproducible if
both sides rounded it identically.

C and C++ permit a compiler to contract `a * b + c` into a single fused
multiply-add, which rounds **once** instead of twice. Whether it does is a
property of the target ISA, not of the source:

| target | baseline FMA? | GCC/Clang at `-O2`+ |
|---|---|---|
| aarch64 (iPad, Android, Apple silicon) | yes (`FMADD`/`FMSUB`) | contracts |
| x86-64, stock | no | cannot contract |
| x86-64, `-march=native` / `-mfma` / `-mavx2` | yes | contracts |

So x86-64 CI was structurally blind to it: the desktop lane could not produce the
divergence it was supposed to be gating. Building the *same sources* on x86-64
with `-march=native` reproduces the arm64 failures exactly, which is the proof
that this is a contraction problem and not an architecture problem.

The effect is not cosmetic rounding noise. Contraction rounds only one side of a
**difference of products**, so an exactly-degenerate pair such as the rasterizer's
barycentric denominator (`d00 * d11 - d01 * d01`) or the ray/triangle determinant
no longer cancels to zero, and the sign it lands on decides an inside test. A
measured bake of `examples/models/spot.obj` lost two covered texels outright.

Contraction buys nothing measurable here — the hot paths are BVH traversal and
memory bound — so determinism wins. An A/B on an FMA-capable build moved neither
a full seven-map bake nor a 20k-quad remesh outside run-to-run noise.

`tests/bake/test_bake_determinism.cpp` pins the rule from inside the binary, so a
build that loses the flag fails with a message naming the cause instead of
leaving a checksum mismatch to be misread as a shading bug.

## Reproducing the arm64 lane without root

Cross-compile natively (fast) and let `CMAKE_CROSSCOMPILING_EMULATOR` run the
result under `qemu-aarch64-static`, so plain `ctest` works with no binfmt
registration and therefore no root. Two packages, `g++-aarch64-linux-gnu` and
`qemu-user-static`, are the whole dependency:

```sh
SYSROOT=/usr/aarch64-linux-gnu       # or wherever the packages were unpacked
cmake -S . -B build/arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
      -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
      "-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-aarch64-static;-L;$SYSROOT"
cmake --build build/arm64 && ctest --test-dir build/arm64 -R '^unit$'
```

Nothing needs installing to `/usr`: both packages can be unpacked into any
directory, as long as `$SYSROOT` and `PATH` point at them. The one fixup a
relocated toolchain needs is in `$SYSROOT/lib/libc.so` and `libm.so`, which are
`ld` scripts holding absolute `/usr/aarch64-linux-gnu/lib/...` paths.

The cheaper first check costs no cross-compiler: build x86-64 with
`-DCMAKE_CXX_FLAGS=-march=native`. It has FMA, so it sees every contraction bug
the arm64 lane sees.

## What is still NOT bit-identical across architectures

Two divergences remain. Neither is reachable by a current test, and neither can
be fixed without moving the x86-64 goldens, so both are recorded rather than
patched.

**1. `std::atan2` in chart re-orientation (`src/uv/src/atlas.cpp`).** glibc's
`atan2f` is not correctly rounded and its x86-64 and aarch64 implementations
disagree by 1 ULP on roughly 0.1% of inputs. `unwrapAtlas` feeds the result to
`cos`/`sin` to rotate a chart to its minimum-area rectangle, so one unlucky chart
shifts the whole packed layout and every bake taken through that atlas. Measured
on `stanford-bunny.obj`; the other five sample models are unaffected, and
`AtlasOptions::reorientCharts = false` makes the bunny bit-identical again.

The clean fix is to drop the transcendental entirely — the rotation by `-angle`
is exactly `(cos, sin) = (ex, -ey) / |e|`, needing only a correctly-rounded
`sqrt` and divide — but that changes the x86-64 UV bits, so it belongs in a
change that re-captures the atlas goldens on purpose.

**2. The remesh pipeline.** `cyberremesh --input spot.obj --target-quads 5000`
is self-deterministic on one machine but produces a different mesh on aarch64
(measured: 1765 of 8749 output lines, one vertex apart). The cross-field solve
uses transcendentals throughout, so this is expected to be the same libm class as
(1), only much wider. The suite's mesh goldens are structural rather than
bit-exact, which is why they stay green on both architectures.

**3. GPU backends.** The rule is applied to `C` and `CXX` only. `nvcc` defaults
to `-fmad=true` and Metal to `-ffast-math`, so a device kernel still contracts;
that is why `accel::IBackend`'s CPU implementation is the reference the parity
tests compare against rather than a peer. Closing it needs a device to measure
on, so it is left as it was.

Bake itself is clean: with an atlas-free UV layout, all seven maps are
byte-identical across x86-64 and aarch64 on all six sample models.
