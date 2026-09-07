# Semantic boundaries and automatic symmetry detection

## Why

Three tasks of `add-zremesher-retopology` (F1, F2, F4) are carried here so that
change can archive with everything it actually shipped. None of them is
abandoned, and none is blocked by the reason originally recorded against it —
both reasons went stale while the track was being built.

**F1 / F2 — semantic and group boundaries.** The original note said a
`ConstraintField` type "would have nothing to carry". That is no longer true:
Phase E shipped the exact mechanism a semantic boundary needs, where a topology
guide is projected to an edge path and tagged as a feature
(`projectGuideToPath` -> `setFeatureEdge`), applied after the dihedral re-tag so
it can never be demoted, and feature edges are already pinned by the seamless
solve. A group or material boundary is the same shape of request: take the edges
where adjacent faces disagree, tag them. No new field type is required.

What is genuinely missing is the **input**. Per-face group / material ids have
nowhere to enter the engine, and `CyberRemeshParams` cannot grow fields without
breaking compiled callers, so this is a sibling ABI entry point plus a decision
about whether the OBJ loader retains `g` / `usemtl`. That is a design decision
with an ABI consequence, which is why it does not belong in a release being
tagged.

**F4 — automatic symmetry detection.** This was sequenced behind forced
symmetry being solid, and forced symmetry now is: the seam residue it was
waiting on is closed, at 0 boundary and 0 non-manifold edges on every model
measured. What remains is the detector's own design rather than the mirror's.

The risk is specific. A detector that fires on a nearly-but-not-symmetric model
applies symmetry silently and lossily, so the threshold and its calibration are
the entire problem. Any implementation must also avoid the checker bug already
recorded in the parent change: quantizing to a tolerance grid and comparing keys
both collides distinct vertices and misses partners across a cell boundary, so
matching must be nearest-within-tolerance. That mistake was independently
re-made once inside this repo, in the first version of `examples/25_symmetry.py`,
which is the measure of how easy it is to write.

## What Changes

- A sibling C ABI entry point carrying per-face group / material ids, and a
  decision on whether the OBJ loader preserves `g` / `usemtl`.
- Group, material and user-preserved boundaries tagged as feature edges through
  the mechanism Phase E already ships, and consumed by field pinning, the
  topology layout and the sizing field.
- Symmetry detection shipped **detect-and-report first** — naming the plane
  without applying it — so the threshold earns confidence before it decides
  anything.

## Impact

- Affected specs: `remeshing-pipeline`
- No behaviour change until implemented; the parent change archives without
  claiming any of it.
