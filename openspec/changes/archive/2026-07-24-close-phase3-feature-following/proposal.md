# Proposal: close-phase3-feature-following

## Why

The `remeshing-pipeline` spec carries Phase 3 as an open gap with a stale scoreboard.
Two things have changed and one needs deciding.

**Validity is now the strongest claim in the project, and the spec undersells it.**
It reads "0 defects on all five models". It is **six**, and the comparison is no
longer close: QuadriFlow tears the cube with **680** defects and stanford-bunny with
32, while the default is 0 on every model at every density measured. This is the only
axis on which the default beats the reference corpus-wide.

**Feature-following should be recorded as a limitation, not an open item.** Eleven
levers have been measured across the post-extraction, gauge, routing, parameterization,
field, pre-remesh and vendored-pre-remesh layers. Two shipped — crease preservation
through the isotropic stage, and planarity-gated crease field-alignment — and both are
confined to fandisk, the one model clearing the 2% native-routing threshold. The one
lever that reaches the vendored backend, where 4 of 6 models actually run, is a count
artifact: lowering the vendored sharp-edge threshold inflates stanford-bunny's quad
count 2644 → 15105 (+471%) rather than aligning anything.

**The 0/5 → 1/6 headline must not be oversold.** The single win is the cube, which
entered the corpus *because* it was already won. No model previously lost is now won.

## What Changes

- **Promote the validity guarantee** to six models with the cube's 680-defect
  comparison, and state explicitly that it is the pipeline's strongest claim.
- **Reframe feature-following** from "known gap" to "known limitation, Phase 3 closed
  on it", with the count-matched per-model figures and the eleven-lever history.
- **Add a reopening bar**: any future proposal must first answer how it reaches the
  vendored backend, rather than proposing another constraint.

## Impact

- Behaviour: none. Documentation and spec only.
- Specs: `remeshing-pipeline` — one requirement modified (two scenarios changed).
