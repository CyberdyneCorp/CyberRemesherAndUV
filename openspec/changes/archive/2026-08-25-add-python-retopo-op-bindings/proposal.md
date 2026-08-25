# Python bindings for the retopology mesh operations

## Why

The C ABI exposes 44 `cyber_retopo_*` entry points; the Python binding reaches
19 of them, and 18 of those are the `selection_*` family. Everything that edits
mesh topology — triangulate, relax, snap-all, delete faces, dissolve edges,
insert loop, merge vertices, rotate edge — is C-ABI-only.

The effect is that a cage a caller builds or subdivides from Python cannot then
be cleaned up, tightened or reprojected there. `Mesh.subdivide` lands quads and
the caller has no way to triangulate them; a snapped cage cannot be relaxed.
Every one of these already exists and is tested in C++.

## What changes

The eight operations that need no stroke geometry become `Mesh` methods. The
stroke and drawing family (`draw_strip`, `create_face`, `create_grid`, the
boundary extenders, symmetry) is deliberately out of scope — it carries its own
geometry and selection model and deserves a separate design pass.

## Impact

Additive. No existing entry point changes. Affected spec: `engine-bindings`.
