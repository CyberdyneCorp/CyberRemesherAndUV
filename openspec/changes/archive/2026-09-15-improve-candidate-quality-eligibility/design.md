## Context

`candidateBeats` currently makes topology eligibility explicit, but it trusts
the numeric score. `scoreQuality` derives its angle value from the median raw
corner angle. A median is robust to noise but unsuitable as the sole shape
quality criterion: it intentionally ignores a small tail of collapsed or
near-flat quad corners.

## Goals / Non-Goals

**Goals:** reject non-finite positions, zero-length edges, empty candidates,
and topology-invalid candidates before aesthetics; use a deterministic tail
statistic for angle quality; preserve stable candidate order on exact ties.

**Non-Goals:** source-surface correspondence, guide continuity, adaptive local
sizing calibration, external score APIs, and corpus-derived weight calibration.
Those require the versioned corpus tracked in #49.

## Decisions

- A candidate is geometry-eligible only when it has at least one face, every
  live vertex has finite coordinates, and every live edge has finite positive
  length above the existing numerical epsilon. Existing boundary-component and
  non-manifold checks remain eligibility conditions.
- Score corner shape using the 95th percentile of absolute deviation from 90
  degrees. This measures the bad visible tail without making one numerical
  outlier the whole score. The prior median remains reporting-only for
  continuity with existing debug output.
- The selector compares eligibility before total score. When both candidates
  are ineligible it keeps deterministic least-bad diagnostics; production
  behavior remains deterministic and no invalid candidate can displace an
  eligible one.

## Risks / Trade-offs

- A 95th percentile is a proxy, not a scaled-Jacobian measurement. The code
  documents that triangulation/Jacobian work is deferred rather than claiming
  artist-quality calibration.
- The score remains an internal candidate heuristic. It is not exposed as a
  cross-platform quality guarantee or a replacement for #49 corpus evidence.

## Migration Plan

No ABI migration is needed. Revert the score implementation and regressions if
the tail statistic proves unsuitable on the acceptance corpus.
