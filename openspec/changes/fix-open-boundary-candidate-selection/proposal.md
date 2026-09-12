## Why

Best-of-two selection correctly treats an open input's boundary as non-defective
in its score, but its final comparator reintroduces raw boundary-edge count as
an absolute defect. A finer valid retessellation can therefore lose to a worse
candidate solely because it has more edges along the same source border.

## What Changes

- Preserve the number of connected boundary components as an explicit
  eligibility condition for candidate selection.
- Continue to reject non-manifold candidates before aesthetic scoring.
- Add deterministic regressions for valid open borders, newly introduced
  boundary components, and closed-input cracks.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `remeshing-pipeline`: Quality-driven selection preserves source boundary
  topology instead of ranking raw boundary tessellation density as a defect.

## Impact

Changes the internal quality-selection API and best-of-two quad-cover path,
plus geometry-analysis tests and the remeshing-pipeline specification.
