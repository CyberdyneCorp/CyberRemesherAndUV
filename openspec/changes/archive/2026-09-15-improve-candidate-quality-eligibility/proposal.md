## Why

The existing best-of-two selector preserves boundary-component count, but a
candidate with non-finite coordinates, zero-length edges, or no usable corner
angles can still be compared as if it were ordinary geometry. Its angle term
uses a median, which can report an excellent value even when a small number of
quad corners are severely distorted.

## What Changes

- Record finite-coordinate and degenerate-edge validity in `QualityScore` and
  reject invalid candidates before aesthetic comparison.
- Replace median-angle scoring with a documented upper-tail absolute deviation
  from 90 degrees, while retaining the median only as a diagnostic.
- Expose the validity and tail-angle values in debug selection output and add
  deterministic score-level regressions.

## Capabilities

### Modified Capabilities

- `remeshing-pipeline`: Candidate selection rejects invalid output geometry and
  evaluates a visible tail of quad-corner distortion.

## Impact

Internal quadrangulation scoring and its unit tests change. No public ABI or
default remeshing parameter changes are introduced.
