# Proposal: curvature map baking

## Why

The community-standard pre-texture recipe — observed verbatim in two
independent practitioner pipelines in the 3DCoat study — is curvature +
occlusion + normal at 4K, baked with a per-target preset, before any painting
happens (in-app or in Substance). We bake normal, AO, displacement and color;
curvature is the missing quarter of the recipe, and every downstream
edge-wear / dirt / cavity material depends on it.

## What Changes

- A **curvature map** joins the bakeable types: signed mean curvature of the
  Target sampled through the existing cage projection, encoded midpoint-gray
  (convex bright, concave dark), plus a **cavity variant** (concavity-only,
  useful directly as a multiply mask).
- Same rules as every other map: resolution up to ≥4096², editable cage,
  component links, GPU dispatch with progress and cancellation, PNG/EXR output.
- CLI and bindings reach the new map type wherever `normal`/`ao` are reachable
  today.

## Capabilities

### Modified Capabilities

- `surface-baking`: curvature/cavity joins the bakeable map types.

## Impact

- `src/bake` (curvature estimation on the Target + sampling), CLI map-type
  flags, bindings, bake report. Additive; no format or ABI breaks.
- Non-goals: baked-lighting maps, thickness maps, multi-channel packed outputs
  (those belong to `add-export-presets` if anywhere).
