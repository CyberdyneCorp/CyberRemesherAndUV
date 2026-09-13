## Context

`Mesh` has stable IDs, face/edge adjacency and typed attributes. The automatic
pipeline returns a new mesh atomically, while `ReferenceSurface` already finds
the closest source face. There is no region extractor, stitcher or public
source-correspondence result.

## Goals / Non-Goals

**Goals:**

- Extract selected faces with deterministic oriented boundary loops.
- Preserve exact border IDs, positions and exterior connectivity.
- Reuse the normal remeshing pipeline only for the extracted region.
- Report source-face/barycentric correspondence and unsupported transfers.

**Non-Goals:**

- Shape-mode borders, brush-defined selection, multi-object editing and a
  generic attribute interpolation framework.

## Decisions

### Return a new mesh instead of rolling back in-place edits

The request names source face IDs and operates on an immutable source mesh.
It returns a new result only after feasibility and stitch validation, matching
the automatic pipeline's atomic contract. In-place rollback would have to
restore deleted element IDs, attributes and host annotations exactly.

### Keep an explicit source-boundary record

Extraction copies selected faces into a compact working mesh and records each
source boundary vertex, oriented loop location and exterior adjacency. The
first delivery accepts only simple manifold interior loops. Source open borders,
holes and branching boundaries are rejected with a reason, not guessed.

### Stitch by source IDs, never positional welding

The region solve receives its border as hard topology guidance. Commit requires
an output loop with the same ordered source-boundary correspondence; the
stitcher reuses those original vertex IDs, copies the untouched exterior and
inserts only the new selected faces. Nearest-position welding could join nearby
sheets incorrectly.

### Make all-quad parity an early feasibility rule

The first policy supports `allQuadsRequired`. Incompatible boundary parity is
reported before the quadrangulator runs. A future residual-polygon policy must
be explicit rather than silently changing the frozen boundary.

### Represent correspondence with closest source-face barycentrics

Retained vertices report identity. New vertices query the original source BVH
and report source face, deterministic triangle barycentrics, distance and
normalized-distance confidence. Queries are restricted to the selected
component and do not cross `group_id`/`material_id` barriers. Nearest-vertex
maps cannot safely interpolate continuous data or represent a seam side.

### Transfer attributes conservatively

Continuous vertex values interpolate from the barycentric hit. Face group and
material values use the hit face. Corner columns are unsupported without an
exact retained-corner mapping and must be named in the report rather than
guessed across a UV seam.

## Risks / Trade-offs

- [The quadrangulator may not retain a requested loop] → validate ordered loop
  equality before commit and return rejection.
- [Nearby sheets can confuse closest-point hits] → filter by component and
  semantic barriers; add a two-sheet regression fixture.
- [The C report needs safe ownership] → use caller-owned count/capacity rows,
  matching the semantic-boundary ABI report.

## Migration Plan

The ABI addition increments the minor version and does not alter existing
remesh or manual-edit calls. Hosts opt in through the new request; failures and
cancellation require no rollback because the source remains unchanged.
