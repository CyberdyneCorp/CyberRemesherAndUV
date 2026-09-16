## 1. Region relax (Auto Relax)

- [x] 1.1 `relaxRegion`: breadth-first ring distance from seeds, smoothstep
      falloff to zero one ring past `rings`, through `relaxSweep`'s existing
      `extraWeight` hook — no second relax kernel.
- [x] 1.2 `cyber_retopo_relax_region`, validating rings, iterations and strength.
- [x] 1.3 Tests: outside the region bit-identical; a two-hop vertex moves at
      rings=2 and not at rings=1; an unconnected sheet 0.05 away never moves;
      pins honoured; no seeds or negative rings changes nothing.

## 2. Loop slide

- [x] 2.1 `faceAlong`: the face traversing a DIRECTED edge, including interior
      edges (build_tools' `faceTraverses` only handles boundaries).
- [x] 2.2 `slideLoop`: one side for every vertex, targets from original
      positions, |t| >= 1 refused.
- [x] 2.3 `cyber_retopo_slide_loop` with `CyberLoopSlideReport`.
- [x] 2.4 Tests: whole loop moves one quarter-rail to one side; a CLOSED ring
      does not twist; t and -t mirror; seed edge does not change the result;
      border edge slides inward only; full rail and NaN refused.

## 3. Bindings

- [x] 3.1 Python: `relax_region`, `slide_loop`, `Symmetry`,
      `snap_symmetry_plane`, `apply_symmetry`, `resymmetrize`.
- [x] 3.2 Swift: `relaxRegion`, `slideLoop`, `MirrorPlane`,
      `snapToSymmetryPlane`, `applySymmetry`, `resymmetrize`.
- [x] 3.3 Remove the symmetry entries from both PENDING_REGISTRATIONS lists.
- [x] 3.4 ABI 1.18 manifest pinned; diff against 1.17 is purely additive.

## 4. Verification

- [x] 4.1 Mutation-verified: C++ 6/6, Python 5/5, Swift 3/3.
- [x] 4.2 Python test passes with numpy made unimportable.
- [x] 4.3 Caches cleared (`__pycache__`, `swift/.build`) and every gate re-run.

## Open decision

- [ ] Whether a slide from an open-border edge should move the whole border row
      (Blender's behaviour) rather than the single edge `edgeLoopFrom` returns.
      Kept consistent with the tag-loop gesture for now; see design.md.

## Session handoff

Two survivors during mutation testing exposed real gaps, both closed: nothing
proved the region relax actually REACHED ring N (a relax stuck at its seeds
passed every "stays put" test), and a boundary-slide assertion compared a mesh
to a snapshot taken after the call, which cannot fail. A third "survivor" was a
harness bug: XCTest prints assertion failures with an `error:` prefix, which a
naive grep read as a compile failure.
