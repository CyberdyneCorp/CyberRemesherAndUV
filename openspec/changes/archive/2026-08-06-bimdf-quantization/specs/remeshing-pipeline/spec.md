# remeshing-pipeline — Bi-MDF quantization

## ADDED Requirements

### Requirement: Global integer quantization

The native seamless solve SHALL support assigning ALL integer isoline counts
through a single global min-deviation-flow optimization over the T-mesh of the
seamless parameterization (Bi-MDF class), as an alternative to per-translation
greedy rounding. Feature-seam integer pins SHALL enter as fixed flow
constraints so feature fidelity is preserved exactly. The quantizer SHALL be
selectable at runtime with the greedy rounding as fallback, and reverting to
greedy SHALL reproduce its output byte-exactly.

#### Scenario: Quantizer swap preserves features

- **WHEN** the same island is solved with the greedy quantizer and the Bi-MDF
  quantizer on a feature-bound CAD mesh
- **THEN** both runs SHALL complete, both SHALL keep every pinned feature
  curve on an integer isoline, and the run report SHALL name the quantizer used

#### Scenario: Degenerate assignment guarded

- **WHEN** the flow optimum would assign a zero isoline count that collapses a
  T-mesh strip
- **THEN** the quantizer SHALL enforce the minimum positive count and report
  the adjustment
