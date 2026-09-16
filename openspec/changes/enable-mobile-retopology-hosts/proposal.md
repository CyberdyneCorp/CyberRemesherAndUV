## Why

An audit of v0.9.0 against eight walkthroughs of the ZBrush Retopology brush,
RetopoFlow 4 and Cozy Blanket found the engine covers nearly every interaction
those apps are built on — and that a mobile host cannot reach most of it.

Three findings drive this change:

1. **Swift binds 110 of 234 C ABI entry points (47%).** All five
   `cyber_snapper_*`, 28 of 47 `cyber_retopo_*`, both guided-remesh entry
   points and the loop queries are unbound. Swift owns the complete stroke
   grammar (13/13) and none of the verbs: a host can classify a stroke as
   `CYBER_ACTION_CREATE_QUAD` and then find no `cyber_retopo_build_face` to
   call and no snapper to snap the result onto. `SoftSelection.swift` already
   accepts a `snapper: OpaquePointer?` that Swift itself cannot construct.

2. **The parity gate cannot see this.** `test_swift_abi_parity.py` checks
   Swift -> header (no phantom symbols, no wrong arity) and never
   header -> Swift, so an entry point nobody bound is invisible to it. The
   `engine-bindings` spec already requires that "a capability the ABI exposes
   but the bindings do not SHALL be recorded as a pending registration"; for
   Swift nothing enforces that. This is the same "green while doing nothing"
   shape as the Windows lane that runs zero Python tests.

3. **Contours has never been implemented.** Draw cross-sections down a limb,
   get a clean tube. Every RetopoFlow walkthrough demonstrates it on arms and
   horns, and there is no entry point, in any language. `extrudeCylinder`
   exists in C++ but is a single band lofted from one supplied ring — not a
   multi-ring loft, and not exposed.

One smaller capability is implemented in C++, required by
`manual-retopology`, and unreachable by any host: **bridge** between two
boundary loops, the "line between two boundary loops with equal vertex count"
gesture the stroke grammar already recognises and nothing can apply.

Pinning was checked and is NOT a gap: `cyber_retopo_relax`,
`cyber_retopo_selection_relax` and `cyber_retopo_selection_transform_pinned`
already take a host-supplied `pinned` list, and Swift already passes it. What a
Swift host cannot do is supply the `snapper` alongside it, which is the same
snapper gap as everywhere else.

Scope note: this change is about the LIBRARY's capability surface. Application
UI/UX is explicitly out of scope — the question being answered is whether a
third party could build a Cozy Blanket-class app on this library, not whether
we ship one.

## What Changes

- Add **Contours**: cross-section rings cut against the Target, span counts
  reconciled between adjacent rings, lofted with consistent seam placement and
  snapped. New C++ entry point and additive C ABI.
- Expose **bridge loops** through the C ABI, so the recognised bridge gesture
  has an operation to apply.
- Bind the **retopology core into Swift**: snapper lifecycle, build face, draw
  strip, contours, boundary fill, surface cut, patch clone, loop operations,
  guided remesh and loop queries.
- Bind the **stroke grammar into Python**, so the gesture path gets regression
  coverage on the harness that can run it.
- Make the parity gate **bidirectional**, with an explicit pending-registration
  list so every unbound entry point is a recorded decision rather than an
  oversight.

## Capabilities

### Modified Capabilities

- `manual-retopology`: Add Contours as a build tool; make bridge reachable by a
  host rather than C++-internal.
- `engine-bindings`: Require binding coverage to be enforced in both
  directions, and name the mobile-reachable retopology surface.

## Non-goals

- Application UI, tool galleries, gesture-to-action routing in a shell.
- Binding UV, baking, image write and export bundles into Swift (0/32 today).
  Real, and a separate change: it is the finishing pipeline, not the drawing
  surface, and mixing the two would make this change unreviewable.
- Measuring the `< 33 ms at 5 M triangles` interactive floor end to end.
