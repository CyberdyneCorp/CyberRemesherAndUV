# Proposal: soft selection

## Why

Manual retopology has tweak, relax and rigid transforms, but no gradient
selection: there is no way to bend an arm, taper a block, or ease a region
into place with falloff. In the 3DCoat corpus this is the Pose-tool selection
model (line / sphere / paint regions with editable falloff), which arrived in
their Retopo room in 2022 after years of user requests — and the taper-by-
line-gradient move is the single most repeated hard-surface technique in the
practitioner videos. The engine already has the pieces (weight-free
`transform_vertices`, `relax`, `snap_all`); this change adds the region-weight
layer and welds snapping into the operation.

## What Changes

- **Gradient region selection** over EditMesh vertices, three sources:
  - **Line**: anchor point → end point; weight ramps 0→1 along the gradient,
    1 beyond the end; optional angle snapping in 15° increments.
  - **Sphere**: center → radius; weight falls off radially with an easing
    curve.
  - **Painted**: per-vertex weights accumulated by brush strokes (adds with
    pressure; subtract mode).
- **Selection operations**: clear, invert, expand, contract, smooth (by 1, 5
  or 10 steps), save/load named selections on the document.
- **Weighted transform**: translate/rotate/scale applied per-vertex scaled by
  weight; weighted relax.
- **Auto re-snap**: during and after a weighted transform the affected
  vertices SHALL stay snapped to the Target — surface glue is part of the
  operation, not a cleanup pass.
- Reachability: C ABI (+ stroke-gesture route for painted mode), Python,
  Swift; app shells consume it for their pose/taper tools.

## Capabilities

### Modified Capabilities

- `manual-retopology`: gradient selection, selection ops, weighted transform.
- `engine-bindings`: the new surface is reachable from all bindings.

## Impact

- `src/retopo` (weight computation, weighted ops), C ABI additions, bindings,
  document persistence for saved selections, tests. Additive.
- Non-goals: face/edge-mode soft selection (vertex weights only, matching the
  proven 3DCoat scope); soft selection in the UV stage (UV islands have their
  own transform model); mask-driven remeshing (that is `add-flow-guides`).
