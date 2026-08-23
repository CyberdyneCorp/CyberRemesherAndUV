# Benchmark harness

Headless scoreboard + CI regression gate for the quad solver: every solver
change lands with a before/after on these metrics. Complements
`examples/10_vs_reference.py` (visual, interactive comparison): this harness
is deterministic, baseline-gated in ctest (`bench` test), and drives external
competitor binaries. Requires `numpy` and `scipy`.

## Commands

```sh
# Benchmark all resolved solvers on the deterministic generated corpus
python3 tools/bench/bench.py run

# Include the downloaded corpus (spot, bob, nefertiti, armadillo)
python3 tools/bench/bench.py run --corpus all --results results.json

# Re-record CI baselines after an intentional solver change (diff = review artifact)
python3 tools/bench/bench.py record

# What ctest runs (test name: bench) — compares against tests/bench/baselines.json
python3 tools/bench/bench.py check
```

## Metrics

| metric | meaning | better |
|---|---|---|
| `quad_ratio` | quads / total faces | higher |
| `singularities` | interior vertices with valence ≠ 4 | lower |
| `hausdorff_p99` / `chamfer_mean` | sampled surface distance to input, normalized by bbox diagonal | lower |
| `angle_dev_mean` / `_p95` | quad corner-angle deviation from 90° | lower |
| `edge_length_cv` | edge-length spread (uniform-density runs) | lower |
| `flow_turning_mean` | mean turning angle (°) along quad edge loops — flow straightness (curved surfaces turn inherently; compare solvers on the same input) | lower |
| `flow_loop_mean_len` | mean quad edge-loop length in steps — global flow coherence | higher |
| `feature_recall` | fraction of input sharp-edge length reproduced by output edges | higher |

Distances are sampling approximations (100k points/side by default) — stable
for trend tracking, not certified bounds.

## Which solver the baselines gate

`baselines.json` records the solver the run used (`native` or `native+geogram`)
and `check` refuses to compare across the two — the build option picks genuinely
different quads, so a baseline recorded on one is not a gate on the other. A
default build resolves only the native solver, so `check` **skips**, which is
easy to mistake for a pass. Look for `bench check SKIPPED` in the log.

To build the gated configuration:

```sh
brew install libomp tbb          # apt: libtbb-dev
cmake --preset cpu-headless -B build/geogram \
  -DCYBER_WITH_QUADCOVER=ON -DCYBER_REQUIRE_QUADCOVER=ON
cmake --build build/geogram
python3 tools/bench/bench.py check --cyber-binary build/geogram/apps/cli/cyberremesh
```

`CYBER_REQUIRE_QUADCOVER=ON` is what makes a missing dependency a configure
error instead of a silent fall back to the native solver — without it you get a
build that skips the gate and looks fine.

Only quality metrics are gated (see `CHECKED_METRICS`); `seconds` is recorded
but never checked, so baselines are not tied to the machine that recorded them.

## External baselines

Competitor binaries are resolved from env vars (see `solvers.json`); unset
solvers are skipped. None are vendored — quadwild is GPL-3 and must stay an
external binary run for comparison only.

```sh
export QUADRIFLOW_EXE=~/tools/quadriflow/build/quadriflow
export INSTANT_MESHES_EXE="~/tools/instant-meshes/Instant Meshes"
export AUTOREMESHER_EXE=~/tools/autoremesher/autoremesher
export QUADWILD_EXE=~/tools/quadwild-bimdf/quadwild   # adjust cmd in solvers.json to your setup
python3 tools/bench/bench.py run --corpus all
```

## Corpus

- *generated* (`corpus.py`): deterministic sphere / sharp box / torus / capped
  cylinder — no network, used by the CI `check`.
- *downloaded* (`corpus.json`): real models with recorded licenses; fetched to
  `.bench-cache/` on first use, sha256-pinned (the fetch prints the hash to pin
  for new entries).
