# Seam-driven unwrap in the bindings

## Why

The manual UV workflow dead-ends at the binding. The C ABI ships 24 seam entry
points — `cyber_seam_set_*` to mark and erase seams, and eighteen
`cyber_seam_path_*` functions for the routed-path tool with its feature-aware
cost model, editable waypoints and commit/undo — and then nothing that consumes
them.

`unwrapIslandToUv`, `packIslands`, `computeIslands` and `stitchAlongSeams` are
C++-only. The one unwrap reachable from C or Python is `cyber_uv_atlas`, whose
`CyberAtlasParams` has no seam field at all: it runs `autoSeams` and computes its
own cuts, ignoring whatever the caller marked.

So a binding consumer can author seams with a well-built tool and then cannot
unwrap along them. `SeamSet`'s own Python docstring calls it "the model gesture
unwrap and sew read" — true in C++, unreachable from the bindings. The README
concedes it in a parenthetical. The effect is that the bindings ship an automatic
atlas generator while advertising the parts of a UV editor.

## What changes

- `unwrapAtlas` accepts an optional caller-supplied `SeamSet`. When present it
  replaces the `autoSeams` stage and the chart-growth and merge options become
  inert; everything downstream — island computation, LSCM with planar fallback,
  re-orientation, packing, and every reported metric — is the code path the
  automatic atlas already uses.
- New C ABI entry points `cyber_uv_unwrap_seams` and its cancellable twin,
  taking a `CyberSeamSet*` and reporting through the existing
  `CyberAtlasResult`.
- New C ABI entry point `cyber_uv_stitch_seams`, so the "sew" half of the model
  the seam set documents is reachable too.
- Python `Mesh.unwrap_seams(seams, params=None)` and
  `Mesh.stitch_seams(seams, edges)`.

## Impact

Additive. No existing entry point changes signature or behaviour, and an
`AtlasOptions` with no seam set is byte-identical to today. Affected specs:
`uv-editing`, `engine-bindings`.
