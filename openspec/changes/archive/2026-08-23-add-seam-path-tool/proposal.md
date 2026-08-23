# Proposal: auto-routed seam paths

## Why

The UV stage specs seam creation as freehand Pencil strokes over edges. That
works on clean topology and fails on exactly the meshes an auto-retopo engine
produces: spiral-looped, irregular layouts where no traceable edge loop exists
— 3DCoat built its UV Path tool for precisely this case, and our own
quad-cover output is the input it exists for. The primitive is already in the
ABI (`shortest_vertex_path`); what is missing is the tool contract: routed
paths, editable before commit, biased toward the edges a seam wants to follow.

## What Changes

- **Auto-routed seam paths**: the user places waypoints; between consecutive
  waypoints the engine routes along mesh edges by least cost, where the cost
  biases toward feature/crease edges and high-curvature valleys and against
  crossing flat regions, so short hops "follow the groove".
- **Editable pending state**: every placed waypoint can be repositioned or
  deleted before commit and the route re-computes; commit turns the whole path
  into seam edges (integrating with the existing seam model and gesture
  unwrap); a resume marker allows continuing from the last committed point,
  and dropping the resume marker never affects committed seams.
- Works alongside — not instead of — stroke-over-edges seams and Erase-to-sew.
- Reachability: C ABI tool ops, Python/Swift, app shells.

## Capabilities

### Modified Capabilities

- `uv-editing`: routed seam paths join seam editing.

## Impact

- `src/uv` or `src/retopo` path routing (edge-cost model over the existing
  shortest-path), pending-path state in the document/tool layer, ABI + 
  bindings, tests. Additive.
- Non-goals: fully automatic seam suggestion (the automatic atlas already
  covers no-input unwrapping); routing across mesh boundaries.
