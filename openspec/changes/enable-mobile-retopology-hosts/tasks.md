## 1. Contours

- [x] 1.1 Fit a least-squares plane to each stroke; refuse a stroke that is a
      point, collinear, or not planar, rather than cutting with an arbitrary
      normal.
- [x] 1.2 Cut the Target with that plane and chain the segments into closed
      cross-section components.
- [x] 1.3 Rank components by distance to the stroke's centroid and take the
      nearest; never merge them.
- [x] 1.4 Resample every ring to one shared span count by arc length.
- [x] 1.5 Align winding before seam, so a reversed ring does not then pick the
      wrong start sample.
- [x] 1.6 Loft consecutive rings; support wrapping the last back to the first.
- [x] 1.7 Report the first unusable stroke by index and leave the mesh
      unchanged, rather than skipping it and lofting across the gap.
- [x] 1.8 Tests asserting properties of the OUTPUT: full-circle rings from a
      partial stroke, a seam that does not spiral, one orientation across every
      band, and each refusal path.

## 2. C ABI

- [x] 2.1 `cyber_retopo_contours` with CSR strokes and a `CyberContourReport`.
- [x] 2.2 `cyber_retopo_bridge_loops`, so the recognised bridge gesture has an
      operation to apply.
- [x] 2.3 Bump the ABI minor to 1.17 and pin the compiler-measured manifest.
- [x] 2.4 Validate offsets are non-decreasing before indexing with them.

## 3. Swift bindings

- [x] 3.1 `Snapper`: create, closest point, nearest vertex, raycast — the handle
      `SoftSelection` was already written against and could not construct.
- [x] 3.2 Build tools: create face, build face (PolyPen), draw strip
      (PolyStrips), contours, bridge, grid, boundary grid/fan, surface cut,
      patch clone.
- [x] 3.3 Flow edits: insert loop, dissolve, rotate, merge, delete, tweak,
      erase, move, distribute path, transform vertices.
- [x] 3.4 Queries: edge endpoints/faces/valence, vertex position, nearest
      vertex/edge, edge loop, quad ring, boundary loop, loop metrics, shortest
      path, topology generation.
- [x] 3.5 Visibility and tagging: hidden faces, tagged edges.
- [x] 3.6 Guided remeshing through the shared `withGuidance` lowering, so the
      quad-cover and ZRemesher paths cannot disagree about what a guide is.
- [x] 3.7 Device resource ceilings, which a mobile host is the caller that must
      set.
- [x] 3.8 Swift package compiles against the built C ABI.

## 4. Parity gate

- [x] 4.1 Add the header -> Swift direction: every declared entry point must be
      bound or registered.
- [x] 4.2 Add `PENDING_REGISTRATIONS` with a reason per entry.
- [x] 4.3 Fail on a stale registration naming a symbol the header dropped, so
      the list cannot rot.
- [x] 4.4 Register the remaining 71 with honest reasons, grouped.

## 5. Follow-ups NOT done in this change

- [ ] 5.1 Bind the finishing pipeline into Swift — UV, bake, image write,
      export bundles (registered as `finishing pipeline`, 32 entry points).
- [ ] 5.2 Bind the stroke grammar into Python, so gestures get regression
      coverage on the harness that can run them (0 of 13 today).
- [ ] 5.3 Bind the renderer fast paths when the Metal viewport stops being a
      scaffold (registered as `renderer fast path`, 9 entry points).
- [ ] 5.4 Symmetry and remaining retopology follow-ups in Swift (registered as
      `retopology follow-up`, 11 entry points).
- [ ] 5.5 Measure the `< 33 ms at 5 M triangles` interactive floor end to end.
      Snapping clearly clears it — raycast cost measured flat at 1.4 -> 1.6 us
      from 90 k to 1.44 M triangles — but recognition and local relax at that
      scale are unmeasured, so the requirement is asserted, not gated.

## Session handoff

Contours verified end to end through the C ABI on a BENT arm (non-parallel
cross-sections): 4 arcs -> 4 rings -> 42 quads, worst ring-vertex distance to
the Target surface 0.000000, and a straight stroke refused with the mesh
untouched.

Swift coverage moved 110/234 (47%) -> 172/236 (72%); `cyber_snapper_*` and
`cyber_stroke_*` are now both 5/5 and 13/13, so a mobile host can recognise a
gesture AND apply it.

Local note, not a regression: `capi_abi_manifest` fails under ctest on this
machine because ctest forces `CXX=${CMAKE_CXX_COMPILER}` (Command Line Tools
`/usr/bin/c++`), which cannot currently link against the installed SDK
(`tapi error: malformed file ... unknown architecture arm64e.x1-macos`). It
fails identically on a clean tree with this change stashed, and the same test
passes when run directly against Xcode's clang++. The 1.17 manifest itself is
correct and pinned.
