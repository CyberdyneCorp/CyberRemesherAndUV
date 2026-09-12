---
name: cyberremesh
description: Common operations for the CyberRemesher quad-remeshing / UV / bake engine — build and test presets, the sanitizer / fuzz / TSan / bench hardening lanes and their host-specific gotchas, the retopology benchmark and its per-toolchain baselines, the OpenSpec workflow, and the release/tag checklist. Use when building, testing, benchmarking, profiling, hardening, spec-ing or releasing this repository.
license: MIT
metadata:
  author: CyberdyneCorp
  version: "1.0"
---

# CyberRemesher operations

A C++20 quad-remeshing / retopology / UV / bake engine with a C ABI, Python and
Swift bindings. `just` is the entry point and every recipe is a thin wrapper
over a CMake preset — prefer it over raw `cmake` so you inherit the right
options.

## Build and test

```bash
just build          # configure + build (preset cpu-headless)
just test           # build, then the full ctest suite
just ctest          # tests only, against an existing build
just format         # clang-format the tree (CI-pinned version)
just format-check   # the CI format gate, non-mutating
just ci             # local CI: test + a GCC build + spec validation
just clean          # remove build*/ and dist/
```

Override the preset for any recipe: `just preset=cpu-headless-debug test`.

**`cpu-headless` sets `-DCYBER_WITH_QUADCOVER=ON`, and that matters.** It builds
the in-process Geogram QuadCover solver, which is the *shipping* default
quadrangulator. A build without it routes to the portable solver and produces
genuinely different quads — different enough that bench baselines from one do
not gate the other. `cyberremesh --version` prints which solver a binary
carries (`native+geogram` vs `native`); check it before trusting any measurement.
It prints three lines — engine version, solver, and `c-abi <major>.<minor>` (the
ABI the binary was compiled against, which is what decides whether a compiled
caller links and moves independently of the engine version).

Run the Python binding tests through ctest, not directly — the bare scripts
print `SKIP: cyber_capi shared library not loadable` because ctest is what
points them at the built `libcyber_capi.so`.

## The hardening lanes

`.github/workflows/hardening.yml` runs nightly (03:17 UTC) and is
`workflow_dispatch`-able. It is NOT on push, because an ASan+UBSan build of the
whole tree plus TSan plus two fuzz campaigns is far too slow for a PR gate.
**Because it is nightly, it rots silently while push CI stays green — check it
before any release.**

```bash
gh run list --workflow hardening --limit 10
gh run view <id> --log-failed
```

Reproducing each lane locally:

```bash
# ASan + UBSan (the `address-undefined` legs). CI runs this TWICE, with
# CYBER_WITH_QUADCOVER OFF and ON, because the preset builds a different
# quadrangulator from the one that ships.
just asan
# The vendored-field leg needs detect_leaks=0: Geogram allocates
# process-lifetime singletons it never frees, so LeakSanitizer reports the
# vendored library rather than this code.
ASAN_OPTIONS=detect_leaks=0 ctest --preset cpu-headless-debug --output-on-failure

# TSan
cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=g++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCYBER_WITH_QUADCOVER=OFF
cmake --build build/tsan
setarch -R ctest --test-dir build/tsan -R 'unit|bridge|fuzz_corpus_replay' --output-on-failure
```

**`setarch -R` is not optional on kernel 6.8+.** Ubuntu 24.04 sets
`vm.mmap_rnd_bits=32`, more ASLR entropy than TSan's shadow mapping can place
itself around, and every TSan binary dies with `FATAL: ThreadSanitizer:
unexpected memory mapping` before `main()`. Locally, disable ASLR for the run
with `setarch -R`; CI lowers the sysctl instead. This is environmental — never
read it as a race.

**Timing assertions must be sanitizer-aware.** A Debug+ASan+UBSan build runs one
to two orders of magnitude slower, so a wall-clock bound tuned on a normal build
fails nightly for no reason. Scale the budget (see `kInflateBudgetMs` in
`tests/imageio/test_load_general.cpp`) rather than deleting the check.

## Fuzzing

The cheap half runs on every CI leg: `fuzz_corpus_replay` replays the checked-in
seed corpus under `tests/fuzz/corpus/{mesh,png}`. The nightly `fuzz` job runs
real libFuzzer campaigns.

**A crash the campaign finds becomes a regression test by being checked in.**
Download the artifact and drop the input into the corpus:

```bash
gh run download <id> -n fuzz-findings -D /tmp/findings
cp /tmp/findings/crash-<hash> tests/fuzz/corpus/mesh/<descriptive_name>.bin
./build/cpu-headless/tests/cyber_fuzz_replay tests/fuzz/corpus   # must be clean
```

The mesh target's first input byte selects the parser (`.obj .ply .stl .gltf
.glb .fbx`), so keep the byte when copying a finding — strip it and you are
replaying a different format.

**Run campaigns locally rather than waiting on the nightly.** The fuzzers build
here and a 15-minute local campaign closes the loop in minutes instead of a
CI queue:

```bash
CC=clang CXX=clang++ cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCYBER_BUILD_FUZZERS=ON -DCYBER_WITH_QUADCOVER=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,fuzzer-no-link -fno-omit-frame-pointer -g"
cmake --build build/fuzz --target cyber_fuzz_png cyber_fuzz_meshIo

# COPY the corpus first -- see below.
cp -r tests/fuzz/corpus/mesh /tmp/wcorpus
./build/fuzz/tests/cyber_fuzz_meshIo /tmp/wcorpus \
  -max_total_time=900 -max_len=65536 -artifact_prefix=/tmp/findings/
```

**Never point a campaign at `tests/fuzz/corpus/` directly.** libFuzzer treats
the corpus argument as an OUTPUT directory and writes every interesting input it
generates into it — one 15-minute run left 1876 untracked files in the
checked-in corpus. Copy it somewhere scratch and fuzz the copy; promote only the
crash artifacts you mean to keep. (The CI job passes the corpus dir directly,
which is fine there because the checkout is thrown away.)

Cleaning up afterwards with `git clean` will also delete any NEW seed you have
added but not yet `git add`ed. Stage the seed first.

**A guard in front of a vendored parser must never fail open.** Four consecutive
fuzz findings against the PLY element-count guard were all the same mistake in
different clothes: the guard deferred ("a header I do not fully understand")
whenever it disagreed with happly, and an adversary controls whether it
disagrees. It must (a) match the parser's own rules — happly matches header
keywords with `startsWith` and reads counts with `istringstream >> size_t`,
which take leading digits and ignore trailing garbage — and (b) keep any
REMAINING disagreement harmless, which here means bounding against the whole
file size when the header end is unknown. A bound no legitimate file can exceed
is always available; use it rather than giving up.

Parsers must never size an allocation from an attacker-controlled count.
`importBinaryStl` is the reference shape (`fileSize != 84 + count * 50`), and
`importPly` pre-flights the header the same way.

## The retopology benchmark

```bash
just bench    # the scored benchmark vs QuadriFlow / AutoRemesher
python3 tools/bench/bench.py check  --cyber-binary build/cpu-headless/apps/cli/cyberremesh
python3 tools/bench/bench.py record --cyber-binary build/cpu-headless/apps/cli/cyberremesh
```

**Baselines are per toolchain**, at
`tests/bench/baselines-<System>-<machine>-<compiler>.json`. The solve reads
unordered-container iteration order, so libstdc++ and libc++ genuinely disagree
(cylinder singularities 4 on macOS/Clang, 6 on Linux/GCC) — that is not a
regression, and widening the tolerance to absorb it would also absorb a real
one. `check` reads only the file matching the current host and SKIPs if there is
none; `record` writes that host's file.

A SKIP is not a pass. The nightly job treats it as fatal on purpose, and the
`bench` ctest case is meaningful only when a baseline for this toolchain exists
and the binary carries the matching solver.

## OpenSpec

```bash
just spec                            # openspec list + validate --all --strict
openspec list
openspec show <change> --json
openspec validate <change> --strict
openspec archive <change> --yes      # merges the delta specs into openspec/specs/
```

Medium or large work gets a change before code. A change archives only when its
tasks are all resolved — and "resolved" legitimately includes *deliberately not
built*, provided the disposition is recorded. Two dispositions used here:

- Work that is real but out of scope is **carried** to a follow-up change, with
  its reasoning copied across (see
  `add-semantic-boundaries-and-symmetry-detection`).
- A task whose own analysis concludes "do not build this" is resolved by acting
  on that conclusion — usually by correcting the spec text so it stops promising
  something that does not exist.

Never archive a change whose delta specs describe unimplemented behaviour: the
archive writes them into the living specs as though they shipped.

## Releasing

Version is single-sourced from `project() VERSION` in `CMakeLists.txt`, and
`packaging/version.sh` is the authority.

```bash
./packaging/version.sh                    # what the tree says
./packaging/version.sh --verify-tag v0.8.0
```

Checklist:

1. Check the nightly hardening lanes are green — push CI passing tells you
   nothing about them.
2. Bump the version in all five places: `CMakeLists.txt`,
   `python/pyproject.toml`, `python/cyberremesh/pyproject.toml`,
   `python/cyberbridge/__init__.py`,
   `python/cyberremesh/cyberremesh/__init__.py`.
3. Promote `## [Unreleased]` in `CHANGELOG.md` to `## [X.Y.Z] - <date>`.
4. `just ci`, and re-record bench baselines if the solver changed deliberately.
5. Archive any completed OpenSpec change so the living specs match the release.
6. Tag `vX.Y.Z` and push it — `release.yml` fires on `v*` tags, verifies the tag
   against `project() VERSION`, and builds the AppImage / dmg / Windows zip.

`v0.5.0` shipped with **no binaries** because the release workflow died on
`-Werror` errors that only appeared on main. A green tag build is part of the
release, not a formality.

## Layout

- `src/` — engine libraries (`core`, `quadrangulate`, `retopo`, `uv`, `bake`,
  `accel`, `imageio`, `net`, `handoff`, `app`, `exportbundle`, `render`,
  `bakecage`)
- `capi/` — the C ABI, exports filtered by `capi/cyber_capi.map`
- `python/`, `swift/`, `blender/` — bindings and DCC integration
- `apps/cli` — the `cyberremesh` binary
- `tests/` — doctest C++ plus Python binding tests, all registered in
  `tests/CMakeLists.txt`
- `tools/bench/` — the benchmark harness; `examples/` — runnable examples and
  the gallery
- `openspec/` — specs and changes; `docs/ROADMAP.md` — measured results and open
  gaps

## Conventions that bite

- **Enum arguments crossing the C ABI.** Reading an out-of-range value *through*
  an enum type is UB and UBSan reports it, so validation cannot be written the
  obvious way. Struct fields dodge it by being declared `int`
  (`CyberFlowGuideEx::mode`); function parameters use the `enumCode` helper in
  `capi/src/capi.cpp`, which takes the argument by reference and reads its
  object representation.
- **A GCC false positive is suppressed at its site**, guarded on the compiler
  and pushed/popped around the single statement — never tree-wide. See
  `sparse_cholesky.cpp` and `quad_extract.cpp`. `build_hygiene` enforces the
  shape of the first one.
- **`build_hygiene` scans the whole repo** for `CMAKE_SOURCE_DIR` /
  `CMAKE_BINARY_DIR`. Vendored trees and dot-directories are excluded; if you
  add a nested checkout somewhere else, exclude it there.
- **Env-var kill switches** (`CYBER_ZR_*`, `CYBER_QC_*`) A/B a lever without a
  rebuild, but a shipped option belongs in the parameter struct — passing it by
  `setenv` was a defect that got fixed once already.
