## REMOVED Requirements

### Requirement: Component links and selective baking
**Reason**: It promised that each EditMesh component bakes only from its linked Target components and that a component flagged to bake alone bakes in isolation. No bake has ever done either: `bake()` takes no link input, and neither the C ABI, the bindings nor the app exposes links. Its scenario "Linked components do not bleed" was never true. It is replaced by "Component link model", which states what exists.
**Migration**: No caller could depend on link-restricted baking, because it never existed. Applying links to the bake is tracked in #103, and it will restore the no-bleed scenario as a tested requirement.

## ADDED Requirements

### Requirement: Component link model
The system SHALL provide a component-link model over the EditMesh and the Target, where a component is a connected face set: explicit high→low links from an EditMesh component to one or more Target components, a per-component "bake alone" flag (the drawing-X gesture), and source resolution that returns a component's explicit links when it has any and otherwise the single nearest Target component. The model SHALL be serializable with the document's cage state.

The bake stage does NOT yet apply this model: every EditMesh component is baked against the whole Target, so an unlinked component, a linked one and one flagged to bake alone all produce the same maps today. Applying the resolved links to a bake — and exposing link editing through the C ABI and the bindings — is tracked in #103. Until then no map type SHALL claim link-restricted sampling, and a new map SHALL introduce no link-specific exception, so that when links are applied they reach every map on the same terms.

#### Scenario: An explicit link wins over the nearest surface
- **WHEN** an EditMesh component has an explicit link to a Target component that is not the nearest one
- **THEN** resolving its source SHALL return the linked component and report the resolution as explicit

#### Scenario: An unlinked component falls back to the nearest Target component
- **WHEN** an EditMesh component has no explicit link
- **THEN** resolving its source SHALL return the single nearest Target component and report the resolution as the fallback

#### Scenario: A bake does not yet honour links
- **WHEN** a bake runs on an EditMesh whose components carry explicit links
- **THEN** every component SHALL be baked against the whole Target, exactly as if no links existed, and nothing in the bake's output or report SHALL suggest otherwise

## MODIFIED Requirements

### Requirement: Curvature and cavity maps
The bake stage SHALL bake a curvature map from the Target onto the EditMesh's
UV layout: signed surface curvature encoded around a midpoint gray, with
convex regions brighter and concave regions darker, normalized by a
user-controllable curvature range. A cavity variant SHALL also be available
that encodes concavity only (flat and convex regions map to white), suitable
for direct use as a multiply mask.

When the curvature range is left at 0 the bake SHALL derive it from the Target
as a percentile of |curvature| weighted by the surface area each sample speaks
for, so a region influences the range in proportion to the area it covers and
not to the number of vertices sitting on it.

Curvature baking SHALL follow the same rules as the other map types: the same
cage projection, output resolution up to 16384² (through the regioned path where the output exceeds the working set),
GPU dispatch with progress reporting and cancellation, and PNG/EXR output.

#### Scenario: Curvature bake distinguishes edges from crevices
- **WHEN** a curvature bake runs against a Target with both sharp convex edges and deep concave seams
- **THEN** the convex edges SHALL read brighter than the midpoint and the concave seams darker, at the requested resolution

#### Scenario: Cavity variant masks concavity only
- **WHEN** a cavity bake runs on the same Target
- **THEN** concave seams SHALL read dark while flat and convex regions read white

#### Scenario: Auto range is not captured by a dense sliver fan
- **WHEN** an auto-ranged curvature bake runs against a Target whose parameterization piles a large share of its vertices onto a vanishing share of its area, such as the sliver fans at a UV sphere's poles
- **THEN** the range SHALL be set by the curvature of the bulk of the surface, leaving the interior detail legible rather than compressed toward the midpoint

#### Scenario: Curvature respects the cage
- **WHEN** the projection cage is edited and the curvature bake re-runs
- **THEN** the sampled regions SHALL follow the edited cage exactly as a normal-map bake would
