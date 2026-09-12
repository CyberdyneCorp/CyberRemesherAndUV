## Context

See proposal.md. `scoreQuality` already knows whether an input was closed, but
`candidateBeats` only receives two scores and unconditionally compares raw
boundary-edge counts. It cannot distinguish a denser valid rim from a crack.

## Goals / Non-Goals

**Goals:**

- Preserve connected boundary-component count during best-of-two selection.
- Make the eligibility decision explicit, deterministic and testable.

**Non-Goals:**

- Establish source-to-output boundary correspondence, measure surface distance,
  recalibrate aesthetic weights, or implement the remaining quality research in
  issue #48.

## Decisions

- Count connected components of the boundary-edge graph. Retessellation changes
  edge count but preserves a component; a new crack normally creates one.
- Pass the input's expected component count to the comparator at the sole
  production call site. Keep a closed-input default for direct utility callers.
- Reject non-manifold edges and component-count mismatches before comparing the
  existing score. This preserves the current deterministic tie-break for two
  eligible candidates.

## Risks / Trade-offs

- [A crack joins an existing boundary without adding a component] → This
  lightweight guard cannot detect it; issue #48 retains the correspondence and
  surface-validity work needed for that stronger guarantee.
- [A legitimate workflow intentionally closes a source boundary] → It is not
  eligible in best-of-two mode; such a behavior needs an explicit cleanup policy
  rather than an incidental candidate preference.

## Migration Plan

No public ABI changes. Roll back by restoring the previous comparator if an
emergency regression is found.
