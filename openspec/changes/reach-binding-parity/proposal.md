## Why

After PR #83 bound the retopology surface into Swift, the two bindings had
drifted into covering **complementary halves of one workflow**, and neither
could run it end to end:

```
bound in BOTH      120
Python only         43    uv, bake, image, export, bundles, handoff
Swift only          52    stroke grammar (13), snapper (3), build tools (14)
NEITHER             21
```

Python could unwrap, bake and export but could not DRAW — 0 of 13
`cyber_stroke_*`, no `build_face`, no `draw_strip`, no `contours`. Swift could
draw but could not FINISH — 0 of 32 for UV, bake and image output. A
Cozy Blanket-class app needs both halves, and today a host has to write C to
get them.

Python also had no real coverage gate. What stood in for one was a function in
`test_abi_contract.py` whose comment claims "engine-bindings requires anything
the C ABI can do to be reachable from Python" and which asserts about six
hand-picked functions. It stayed green with 71 entry points unbound — including
the whole gesture grammar, on the binding that exists to be the full-surface
desktop test harness.

## What Changes

- Factor the header -> binding coverage check into one shared module and run it
  against BOTH bindings. Python gains the gate it never had.
- Detect a third way the registration list can lie: an entry registered as
  pending that the binding actually binds. Harmless to the build and corrosive
  to the list, which a reader trusts to say what is missing.
- Bind the drawing surface into Python: stroke grammar, snapper queries, build
  tools, and element/loop/picking queries.
- Bind the finishing pipeline into Swift: UV atlas and seam unwrap, baking, and
  image output.
- Tests that exercise each binding end to end, mutation-verified.

## Capabilities

### Modified Capabilities

- `engine-bindings`: The coverage gate SHALL run for every binding, and SHALL
  fail on a pending registration for an entry point the binding binds.

## Non-goals

- Cancellable UV variants in either binding (they need a job/trampoline wrapper,
  which is a behaviour change, not a binding).
- Renderer fast paths (borrowed pointers into engine buffers).
- Export bundles in Swift (a desktop DCC hand-off a sandboxed host does not
  drive).
