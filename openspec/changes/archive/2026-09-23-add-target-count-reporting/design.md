## Decisions

- Count is measured after each extractor attempt and again after pipeline
  cleanup/pure-quad expansion. These are distinct facts and neither implies
  the other.
- The default remains the existing two-attempt calibration and selects the
  closest eligible incumbent by absolute log count ratio. A policy is opt-in;
  it bounds attempts rather than assuming count is monotone and using bisection.
- A closer candidate cannot replace an incumbent if it violates required
  geometry, boundary, or topology validity. A miss is a successful run with a
  distinct report reason, not cancellation or an error.
- Per-island allocation is recorded from the pipeline's effective target. The
  aggregate record names requested and final totals so hosts never infer one
  from the other.
- ABI parameters are introduced in a new sibling entry point and reports are
  returned through caller-owned output structs filled only on success. Existing
  `CyberRemeshParams` layouts remain unchanged.

## Non-goals

- Exact count guarantees on arbitrary constrained topology.
- Changing the default count calibration, quality ranking, or pure-quad
  construction.
- Replacing the extractor's quality/candidate-selection policy.
