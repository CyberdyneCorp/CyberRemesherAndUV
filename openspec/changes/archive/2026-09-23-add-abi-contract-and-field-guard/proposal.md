# An ABI version contract, and a guarded field-evaluator boundary

## Why

The project's direction is to **be the engine other people embed**. Two things
stood between that and a host being able to depend on this library, and neither
was new scope — both were requirements or invariants the repo already implied.

**1. The ABI had no version.** `engine-bindings` has required one since the
bootstrap change: *"The ABI SHALL carry a runtime-queryable semantic version;
minor releases SHALL be additive only"*, with a `Scenario: ABI version query`
for a 1.x client loading a 1.y library. Nothing implemented it. `cyber_version()`
reports the ENGINE's version, which moves for quality work that leaves the
surface untouched, and `SOVERSION` tracked the project major — 0 for every
release, so `libcyber_capi.so.0` named v0.7.0 and v0.8.0 alike. The scenario
could not even be written as a test: there was no ABI version to compile against.

That is not hypothetical. v0.5.0 → v0.6.0 grew `CyberAtlasResult`, an **out**
param, so the callee wrote two fields past an older caller's buffer — under an
unchanged soname. And the first external embedder pinned by commit and
hand-rolled its own expected-ABI constant, because the library offered nothing
to assert against.

**2. The field-evaluator boundary failed open.** `CyberFieldEvaluator` is the
one place HOST code returns values into ours, and of its callbacks only
`occlusion` was defended (a silent clamp). A NaN distance defeated the guard
completely: `std::fmax(NaN, epsilon)` is `epsilon`, so the march neither hit nor
stopped — it spent all 96 steps and returned a miss indistinguishable from empty
space. A non-finite gradient reached `normalized()`, which maps it to `{0,0,0}`,
and a zero "normal" was encoded into the map as though measured. The C ABI
pre-seeded the gradient out-param with `{0,0,1}`, so a callback that returned
without writing — the ordinary shape for a grid field asked outside its domain —
produced a plausible +Z normal. The Python trampolines swallowed exceptions and
substituted `0.0`, which satisfies `|d| <= epsilon` and therefore reported a HIT
at the cage origin.

Every one of those hands back an image that looks like a real map. That is the
same fails-open pattern the PLY element-count guard was fixed for.

## What Changes

- An ABI version distinct from the engine version, declared in the header as
  `CYBER_ABI_VERSION_MAJOR` / `_MINOR` (so bindgen and a source-vendoring
  consumer both see it), queryable via `cyber_abi_version`, with the
  compatibility rule implemented ONCE in `cyber_abi_check` so every binding
  agrees. `SOVERSION` becomes the ABI major.
- A two-tier field-evaluator boundary. TIER 1, which no correct field can
  produce, fails the whole bake with a message naming the callback and the point.
  TIER 2, which a correct field does hit, is a counted miss.

## Impact

- Affected specs: `engine-bindings`, `surface-baking`
- `SOVERSION` moves 0 → 1: a one-time packaging change, taken while the blast
  radius is one constant in the Python loader.
- No behaviour change for a well-behaved field; the existing field-bake suite
  pins that and is unmodified.
