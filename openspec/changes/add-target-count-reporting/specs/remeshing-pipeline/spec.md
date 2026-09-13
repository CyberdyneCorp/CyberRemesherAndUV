## ADDED Requirements

### Requirement: Count allocation and final-count reporting

The pipeline SHALL record each island's allocated effective target and its
achieved final count, including any pure-quad expansion, and SHALL report the
whole-run requested and final totals without representing a request as a
guarantee.

#### Scenario: Pure-quad expansion

- **WHEN** pure-quads changes the final face count after calibration
- **THEN** the report SHALL expose both calibrated and final counts
