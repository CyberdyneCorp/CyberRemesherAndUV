## Why

Three capabilities the `manual-retopology` spec already requires were either
unreachable by any host or, in one case, implemented against the spec:

- **Auto Relax.** The spec requires "an automatic local relax of surrounding
  topology" after every topology-modifying operation — "the new and neighboring
  vertices". `commands.hpp` has an `autoRelax()`, but it relaxes the WHOLE mesh
  (`brushRadius = 0`). Exposed as-is it would ship a spec violation: run after
  every stroke it moves topology the artist placed carefully on the far side of
  the model, and at 100k EditMesh vertices it spends the interactive frame on
  vertices nobody touched. It also has zero C ABI presence.
- **Loop slide.** The gesture grammar names it ("double-tap an edge loop to
  slide it"), so the recogniser can name an action nothing can perform. C++ has
  only `slideVertex`, which moves ONE vertex toward a neighbour the caller names.
- **Interactive symmetry.** `cyber_retopo_apply_symmetry`, `_resymmetrize` and
  `_snap_symmetry_plane` are in the C ABI and bound in NEITHER binding, both
  registered as "retopology follow-up". Draw on one side, get the other, is table
  stakes for character work.

These are the difference between a toolset that works and one that feels
right, which is what Cozy Blanket and RetopoFlow users actually describe.

## What Changes

- Add a TOPOLOGICAL region relax — `rings` edge hops from seed vertices, smooth
  falloff, bit-identical outside — as the correct implementation of Auto Relax,
  and expose it as `cyber_retopo_relax_region`.
- Add a loop slide that moves every vertex of an edge loop toward the SAME side,
  and expose it as `cyber_retopo_slide_loop`.
- Bind the three existing symmetry entry points into Python and Swift.
- ABI 1.18, additive: two entry points and one report struct.

## Capabilities

### Modified Capabilities

- `manual-retopology`: Auto Relax is scoped to the edit's topological
  neighbourhood; loop slide and interactive symmetry are reachable by a host.

## Non-goals

- An engine-held "auto relax is on" mode applied inside every build op (see
  design.md for why).
- Changing `edgeLoopFrom`'s definition of a loop so border rows count as loops.
- Removing or changing the existing whole-mesh `autoRelax()`, which callers use
  as relax-all.
