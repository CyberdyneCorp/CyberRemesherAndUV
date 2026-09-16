## 1. Shared coverage gate

- [x] 1.1 Factor the header -> binding direction into `binding_parity.py`.
- [x] 1.2 Move the Swift gate onto it.
- [x] 1.3 Add `test_python_abi_parity.py`, tokenizing to strip comments and strings.
- [x] 1.4 Fail on a pending registration the binding already binds.
- [x] 1.5 Register `python_abi_parity` in CTest.

## 2. Python drawing surface

- [x] 2.1 Stroke grammar (13/13) as `cyberremesh.strokes`, with typed enums and
      an immutable `Interpretation`.
- [x] 2.2 Snapper queries: snap to surface, snap to vertex, raycast.
- [x] 2.3 Build tools: create/build face, draw strip, contours, bridge, grid,
      boundary grid and fan, grow boundary edge, surface cut, patch clone,
      tweak, erase, move, distribute path, transform vertices.
- [x] 2.4 Queries: live faces, triangle count, edge faces and valence, nearest
      vertex/edge, edge loop, quad ring, boundary loop, shortest path, loop
      metrics, hidden faces, tagged edges, normal/edge/triangle copies.
- [x] 2.5 `status_string` from the engine, and format-explicit `load_obj`/`save_obj`.
- [x] 2.6 `test_draw_surface.py`, exiting 77 on skip so CTest cannot report a
      lane that never loaded the engine as passed.

## 3. Swift finishing pipeline

- [x] 3.1 UV atlas, seam unwrap and seam stitching with engine-sourced defaults.
- [x] 3.2 Baking into an owning `Image`, with pixel copy and PNG output.
- [x] 3.3 `RetopologyWorkflowTests`: snap -> contours -> unwrap -> bake -> PNG
      through one binding.

## 4. Verification

- [x] 4.1 Mutation-verify the Python tests: 6 of 6 caught, after tightening the
      two that initially survived (symmetric ray, geometry-only timing).
- [x] 4.2 Mutation-verify the Swift tests: 4 of 4 caught, after adding a
      non-default-parameter test.
- [x] 4.3 Clear every `__pycache__` and `swift/.build` and re-run from clean.

## Session handoff

Coverage: Python 163 -> 214 of 236, Swift 172 -> 178 of 236. Both gates pass.

A methodology trap worth recording, found while doing 4.1. Restoring a mutated
source file does NOT restore its bytecode: Python invalidates `.pyc` by source
size plus whole-second mtime, so a same-length mutation (swapping two arguments)
restored with `cp` inside the same second leaves the mutant live in
`__pycache__`. `diff -q` on the source says clean and `inspect.getsource` shows
correct code while the interpreter runs the mutant. It surfaced as a genuine
ctest failure hours later — the tightened raycast test catching the leftover
swap. Step 4.3 exists because of it.
