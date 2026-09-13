## 1. Region contract and feasibility

- [ ] 1.1 Add typed core request/result, boundary mode and explicit rejection
  reasons without changing existing remesh parameters.
- [ ] 1.2 Extract selected face components and oriented simple interior loops;
  reject empty, dead, disconnected, open, holed, branching and non-manifold
  selections before solving.
- [ ] 1.3 Implement exact all-quad boundary parity feasibility and core
  regressions proving failure leaves the input unchanged.

## 2. Region solve and atomic stitch

- [ ] 2.1 Lower extracted boundaries to hard topology constraints and solve the
  isolated region against its original source surface.
- [ ] 2.2 Validate ordered output-loop correspondence and stitch by original
  boundary IDs while copying exterior faces unchanged.
- [ ] 2.3 Validate output manifoldness, orientation and seam uniqueness before
  returning; add selected-patch and invalid-loop regression fixtures.

## 3. Correspondence and attributes

- [ ] 3.1 Return retained-ID and source-face/barycentric/distance/confidence
  correspondence for created vertices, filtered by component and semantic
  barriers.
- [ ] 3.2 Transfer continuous vertex and categorical face attributes; list
  unsupported corner and unknown columns in the report.
- [ ] 3.3 Add analytic-surface, nearby-sheet, material-barrier and UV-seam
  regression tests.

## 4. Public API and document integration

- [ ] 4.1 Add versioned C ABI input/report entry points and regenerate the ABI
  manifest with buffer-capacity regression tests.
- [ ] 4.2 Expose typed Python and Swift APIs with parity and cancellation tests.
- [ ] 4.3 Commit a successful result as one undoable document operation;
  rejected/cancelled results create no undo entry.

## 5. Verification and documentation

- [ ] 5.1 Update README, roadmap and binding documentation with frozen-border,
  feasibility, correspondence and unsupported-attribute semantics.
- [ ] 5.2 Run strict OpenSpec validation, focused core/C/Python/Swift tests and
  the full configured CTest suite.
