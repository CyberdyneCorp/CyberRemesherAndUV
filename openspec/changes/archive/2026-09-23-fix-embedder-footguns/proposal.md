# Fix the three embedder footguns

## Why

Three things in the consumer-facing surface were correct but shaped so that a
host gets them wrong quietly. All three produce a plausible result rather than
an error, which is the failure mode worth spending effort on.

**The AO polarity trap.** `CyberFieldEvaluator::occlusion` returns *openness*,
and `CYBER_BAKE_AO`'s comment read "ambient occlusion / openness" — naming both
and committing to neither. A host that measures occlusion and passes it straight
through bakes an inverted map: light where it should be dark, and plausible
enough to ship. The C++ and Python override points were named for the inverse of
what they return, so an implementer following the name computed the wrong
quantity.

**Element-id stability was prose only.** The rules in `cyber_capi.h` are exact —
`snap_all` keeps every id, `subdivide*` reassigns all of them, `triangulate`
keeps vertices and splits faces — but nothing let a host ASK. A stale annotation
(a pin, a tag, a mapping back into the host's own scene) silently pointed at
whatever now holds that index.

**No import resource ceiling.** Hostile input is refused structurally: a declared
element count is checked against the bytes the file carries. But a *legitimate*
200M-vertex file was simply attempted, and the engine offered a host no way to
say "not on this device". The first external embedder wrote its own cap and
documented it as staying under "the engine's 50M default" — a number that does
not exist here.

## What Changes

- `openness` is the name of the C++ and Python override points, with a
  forwarding shim (and a warning) for subclasses still saying `occlusion`. The C
  ABI field name is frozen by ABI 1.0 and keeps it; its documentation now states
  the polarity where the map is chosen.
- `cyber_mesh_topology_generation()` — a monotone counter that moves exactly
  when the documented rules say ids may have been reassigned.
- `cyber_set_max_import_vertices()` — an opt-in resource ceiling, off by
  default, documented as distinct from the hostility bound.

## Impact

- Affected specs: `engine-bindings`, `mesh-io`
- **ABI 1.0 → 1.1**, which is the additive-only rule exercised on itself: two
  new entry points, nothing removed or reshaped, the soname unchanged, and a
  client compiled against 1.0 still served.
- Source break for a C++ implementer of `FieldEvaluator` (a pure virtual is
  renamed). Python is shimmed; the C ABI is untouched.
