# Proposal: flow guides and painted density

## Why

The remeshing pipeline is fully automatic: the only controls are global
parameters. 3DCoat's AUTOPO — the accepted UX for controllable auto-retopo —
gives artists two local controls that ours cannot express: **stroke flow
guides** (draw the direction edge loops should follow, e.g. around a shoulder
joint) and **painted density regions** (more polygons where painted, e.g.
faces and hands). A field-based pipeline can honor both more principledly
than 3DCoat's implementation: guides become alignment constraints on the
cross field, density becomes a sizing multiplier.

This is deliberately distinct from the descoped automatic adaptivity
(quality-campaign Phase 2, retracted by measurement): nothing here is
inferred — both inputs are explicit, user-drawn, and default to absent.

## What Changes

- **Flow guides**: the pipeline accepts polyline guides (ordered points on or
  near the Target surface). Each guide contributes a soft alignment constraint
  to the cross-field smoothing — field directions near the guide prefer its
  tangent — with a per-guide strength and influence radius.
- **Painted density**: the pipeline accepts a per-vertex (or per-face) scalar
  on the Target that multiplies local target edge sizing; 1.0 everywhere means
  today's behavior, byte-identically.
- **Loud routing**: if the selected backend cannot honor guides or density for
  an island, the per-island report says so — guidance is never silently
  dropped.
- Reachability: C ABI, Python, network bridge (guides drawn in a DCC or the
  app shell), CLI (guides/density from a sidecar file).

## Exit gate

On a guided test corpus: near each guide, extracted loop direction agrees
with the guide tangent within a stated angular tolerance (target ≤15° mean
within the influence radius); the unguided corpus metrics (median angle,
singularity counts, validity) do not regress.

## Capabilities

### Modified Capabilities

- `remeshing-pipeline`: guides and density join the pipeline inputs.
- `remeshing-parameters`: per-guide strength/radius and density clamps join
  the validated parameter surface.

## Impact

- Cross-field build (constrained smoothing term), sizing stage, per-island
  reporting, bindings, bridge protocol message, CLI sidecar. No change to
  unguided behavior.
- Non-goals: automatic curvature-driven adaptivity (stays descoped); guides
  as hard constraints (soft only — hard constraints re-open the singularity
  campaign); UI for drawing guides (app/bridge concern).
