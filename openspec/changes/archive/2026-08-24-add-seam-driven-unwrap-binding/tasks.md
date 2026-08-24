# Tasks: seam-driven unwrap in the bindings

## 1. Engine

- [x] 1.1 `AtlasOptions` gains an optional caller-supplied `const SeamSet*`; when set, `unwrapAtlas` uses it instead of running `autoSeams`, and the growth/merge stages are skipped
- [x] 1.2 The no-seam-set path stays byte-identical to today (the automatic atlas is unchanged)

## 2. C ABI

- [x] 2.1 `cyber_uv_unwrap_seams` + `cyber_uv_unwrap_seams_cancellable`, taking `const CyberSeamSet*` and reporting through the existing `CyberAtlasResult`
- [x] 2.2 `CyberUnwrapSeamsParams` carrying only the knobs that still mean something once the seams are given (pack margin, texture size, re-orient), with a defaults filler like the atlas has
- [x] 2.3 `cyber_uv_stitch_seams` over the same seam set
- [x] 2.4 A null seam set is `CYBER_ERR_INVALID_ARG` naming the argument, never a silent fall back to automatic seaming
- [x] 2.5 Version script needs no change: `capi/cyber_capi.map` exports `cyber_*` by wildcard, so the new entry points are covered

## 3. Python

- [x] 3.1 Declare the new entry points in `_ffi.py`
- [x] 3.2 `Mesh.unwrap_seams(seams, params=None)` returning the existing `AtlasResult`, and `Mesh.stitch_seams(seams, edges)`
- [x] 3.3 Export `UnwrapSeamsParams` from `__init__.py`. Also bound `cyber_mesh_edge_count` and added `Mesh.edge_between`: marking a seam takes an edge id and the binding had no way to enumerate or find one, which is part of the same gap

## 4. Tests

- [x] 4.1 C++: charts follow the marked seams; an empty seam set is one chart; growth/merge options are inert when seams are supplied; the automatic path is unchanged
- [x] 4.2 C++: sewing an edge removes the cut and rejoins the islands
- [x] 4.3 Python: a routed `SeamPath` committed into a `SeamSet` drives an unwrap end to end — the gap this change exists to close
- [x] 4.4 Python: null/empty argument handling and the cancelled-before-any-write contract

## 5. Docs

- [x] 5.1 README: drop the parenthetical admitting the gap, and show the seam → unwrap path
- [x] 5.2 CHANGELOG entry
