# Proposal: ClayCore pipeline bridge

## Why

The 3DCoat study's central finding: 3DCoat's moat is not any single tool but
the sculpt → retopo → UV → bake pipeline living in one place. Our stack has
state-of-the-art engines on both sides — ClayCore sculpts, this engine
remeshes/unwraps/bakes — and **no seam between them**. ClayCore's roadmap
(its Phase 3) commits to providing a retopo-oriented export profile and a
field-evaluation callback; this change is the receiving half. Together they
turn two libraries into a product story no native app currently offers.

The bridge is an interchange, not a merge: this engine keeps **zero hard
dependency on ClayCore**. Everything arrives through a versioned handoff
format and an abstract evaluator interface that any volumetric engine could
implement.

## What Changes

- **Target ingest from a sculpt handoff**: accept a ClayCore "for-retopo"
  export — positions, normals, vertex colors, material-mix attribute — as a
  bake/retopo Target, via file handoff (PLY/GLB profile) and, on-device, an
  in-memory buffer handoff with no intermediate file.
- **Field-sampled baking**: an optional `FieldEvaluator` interface (sample
  distance, gradient/normal, AO at a point). When a bake has an evaluator,
  normal/AO/curvature sampling queries the field directly — exact gradients
  instead of mesh raycasts; without one, baking falls back to today's
  raycast path with identical output contracts.
- **Conform**: given an updated Target (the sculpt changed after retopo
  started), re-snap the existing EditMesh to the new surface preserving
  topology, and report the maximum and RMS deviation — never silently
  stretch.
- **CLI composition first**: `cyber remesh --target <handoff> --preset
  game-50k --bake normal,ao,curvature` consumes a `clay export --for-retopo`
  product; the report records the handoff version.

## Capabilities

### New Capabilities

- `pipeline-bridge`: the handoff contract, evaluator interface, and conform
  operation.

### Modified Capabilities

- `cli-headless`: target-handoff input and report coverage.

## Impact

- `src/io` (handoff reader), `src/bake` (evaluator-backed sampling),
  `src/retopo` (conform over `snap_all` with deviation reporting), CLI,
  bindings. Additive; the evaluator is an interface so builds without
  ClayCore are unaffected.
- Depends on: ClayCore's export-profile half (its Phase 3). The handoff
  format version gates compatibility loudly on both sides.
- Depends on (soft): `add-curvature-bake` and `add-export-presets` make the
  bridge's one-command exit maximally useful but are not blockers.
- Non-goals: a shared document container (a later iteration), live
  bidirectional sync (the network bridge covers interactive interchange),
  linking ClayCore into this engine.
