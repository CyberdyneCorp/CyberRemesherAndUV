# Fuzz harnesses and seed corpus

Two targets over the untrusted-input surface the hardening rounds worked on:

| Target | Entry point | Under test |
|--------|-------------|------------|
| `png`    | `cyber::fuzz::png` (`fuzz_png.cpp`)         | `cyber::imageio::loadPng` |
| `meshIo` | `cyber::fuzz::meshIo` (`fuzz_mesh_io.cpp`)  | `cyber::io::importMesh` (OBJ/PLY/STL/glTF/GLB) |

Both parsers take a path, so the harness writes each input to a temp file
(`temp_input.cpp`) and deletes it afterwards. `meshIo` consumes its **first byte**
as a parser selector (`.obj .ply .stl .gltf .glb`), which is why the seeds under
`corpus/mesh` all carry a leading selector byte and are named `*.bin`.

## Replay (runs in CI, every platform)

`cyber_fuzz_replay` feeds every checked-in seed through its target. It needs no
clang and no libFuzzer, and it is bounded — 1 MiB per seed, 512 seeds, a 60 s
budget per corpus directory, and a 120 s ctest timeout — so a regression that
hangs or allocates without bound fails the run instead of wedging the runner.
An empty corpus directory is a failure too: a vacuous pass is what let the
original campaign findings go unguarded.

```sh
ctest --preset cpu-headless -R fuzz_corpus_replay
```

## Campaign (clang, on demand)

```sh
CXX=clang++ cmake -S . -B build/fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCYBER_BUILD_FUZZERS=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,fuzzer-no-link -fno-omit-frame-pointer"
cmake --build build/fuzz --target cyber_fuzz_png cyber_fuzz_meshIo
./build/fuzz/tests/cyber_fuzz_png  tests/fuzz/corpus/png  -max_total_time=600
./build/fuzz/tests/cyber_fuzz_meshIo tests/fuzz/corpus/mesh -max_total_time=600
```

The `CMAKE_CXX_FLAGS` are what give the *libraries* coverage instrumentation;
the per-target `-fsanitize=fuzzer-no-link` only covers the harness translation
units.

## Adding a seed

Minimise the reproducer first (`-minimize_crash=1`), drop it into
`corpus/png` or `corpus/mesh` with a name that says what it exercises, and keep
it under 1 KiB where possible. Every crash a campaign finds should land here in
the same commit as its fix, so the replay case is the regression test.
