# remeshing-parameters — guide and density parameters

Delta for `add-flow-guides`.

## ADDED Requirements

### Requirement: Guide and density parameters are validated
The parameter surface SHALL validate per-guide strength and influence radius
and the global density clamp range, with documented defaults and the same
clamp-and-report behavior as existing parameters. The effective post-clamp
values SHALL appear in the machine-readable report.

#### Scenario: Out-of-range guide strength clamps and reports
- **WHEN** a run supplies a guide strength beyond the documented range
- **THEN** the value SHALL clamp and the report SHALL record the effective value
