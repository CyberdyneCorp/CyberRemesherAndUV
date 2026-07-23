# Tasks: align-quality-claims-with-measurement

Spec-correctness change — no engine work. The measurements already exist; these
tasks are about making the spec state them.

## 1. Establish the measurement

- [x] Re-point the benchmark at the shipped extractor (`7abbaec`) — the
      comparison phases had been scoring the retired `instant-meshes` path.
- [x] Re-measure the corpus at `--target-quads 3000` on the `cpu-headless`
      build (`-DCYBER_WITH_QUADCOVER=ON`): per-model median, irregular %,
      edge-length CV, defects, feature error.
- [x] Record the standing table in `docs/ROADMAP.md`.
- [x] Re-measure the cube tear case (`examples/12_cad_robustness.py`) rather than
      quoting it from notes — the figure was remembered two ways (554 and 508)
      and is actually **576** boundary edges at ~2100 quads, against our 0. It
      scales with requested density, so the spec states the shape of the result
      and pins the density it was measured at.

## 2. Correct the spec

- [x] Split the overloaded default-extractor scenario: selection/degrade
      behaviour stays, the quality claim moves to its own scenario.
- [x] Narrow the quality guarantee to the measured majority (spot, rocker-arm,
      stanford-bunny), naming fandisk and cheburashka as known exceptions.
- [x] Add topological validity as the corpus-wide (5/5) guarantee, including the
      cube case where QuadriFlow tears.
- [x] Add feature-following as an explicit known gap, with the root cause
      (un-pinned integer-grid phase) so the fix is not mis-scoped as a snap.
- [x] Note that the position-field extractor is retired from comparison claims —
      the mis-measurement that produced the original overclaim.

## 3. Verify

- [x] `openspec validate align-quality-claims-with-measurement --strict`.
- [x] `openspec archive align-quality-claims-with-measurement` — merged into
      `openspec/specs/remeshing-pipeline/spec.md`; 14/14 specs validate strict.
