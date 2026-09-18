## Why

CyberTexel (issue #86) parameterises its generators, smart masks and smart
materials by MESH MAPS rather than by pixels — that is what lets a smart
material re-derive itself on a new model. It ships no baker; this repository
does the baking. Four maps in its `mesh-maps` set have no counterpart in
`surface-baking` today:

- **Object-space normal** — the surface normal in the mesh's own space, encoded
  `n * 0.5 + 0.5`, with a selectable up axis.
- **Object-space position** — the same hit point `BakeMap::Position` already
  writes in model units, rescaled over a bounding box so it lands in `[0,1]`.
- **Bent normal** — the average direction of the unoccluded hemisphere samples
  the AO baker already fires.
- **Thickness** — how much material sits behind the surface, measured by casting
  the AO hemisphere from the INVERTED normal.

Two of the four are ray-traced and belong beside the AO baker that already
exists here; the other two ride the cage projection every other map uses.

## What Changes

- Four new `BakeMap` values: `ObjectNormal`, `ObjectPosition`, `BentNormal`,
  `Thickness`, each requestable through every entry point AO is: the C ABI, the
  export presets and the CLI's `--bake`, Python and Swift.
- Three new `BakeParams` members with stated defaults and ranges: `upAxis`
  (y-up | z-up, default y-up), `bentNormalSpace` (tangent | object, default
  tangent) and `thicknessScale` (default 2.0 — ArmorPaint's doubling, made a
  parameter instead of an inherited constant).
- **A bake now reports its ENCODING BASIS.** `BakeResult::encoding` says what the
  numbers mean: the frame a direction is in, the up axis it was re-expressed in,
  the bounding box a position was rescaled over, the factor a distance was
  multiplied by. It reaches a host through `cyber_image_encoding`,
  `cyber_bundle_result_file_encoding`, both bindings, and the CLI's JSON report.
  Without it an object-space position map is a picture of some numbers.
- The ray-traced pass (AO, bent normal, thickness) now reports progress as it
  accumulates, instead of a single report at the end.
- ABI 1.19, additive: four enum values, three appended `CyberBakeParams`
  members, two appended `CyberBundleParams` members, `CyberImageEncoding` and
  two accessors.

## Capabilities

### Modified Capabilities

- `surface-baking`: the bakeable map set gains object-space normal, object-space
  position, bent normal and thickness; every baked map now carries a recorded
  encoding basis.

## Non-goals

- Redefining `BakeMap::Position`. It means "the hit point in model units" and
  existing callers read exactly that; the new encoding is a new map type
  (design.md).
- Field-evaluator support for the four new maps. They describe the Target MESH,
  like `Displacement`, `Position` and `Color`, which the field path already
  excludes.
- RGBA channel packing (ORM/MER). Explicitly CyberTexel's `texture-export`.
- UV border dilation of the new maps, which is issue #90 for every map at once.
