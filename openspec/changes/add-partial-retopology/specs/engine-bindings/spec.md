## ADDED Requirements

### Requirement: Partial retopology is binding-complete

The C ABI SHALL expose selected-region partial retopology through additive POD
input and report types. Python and Swift SHALL expose the same frozen-border
mode, result status, correspondence and unsupported-attribute diagnostics.
Cancellation, invalid selection and failed feasibility SHALL leave the caller's
input mesh unchanged.

#### Scenario: Binding reports frozen-border result

- **WHEN** a Python or Swift host retopologises a selected region with exact
  frozen borders
- **THEN** it SHALL receive the output mesh and a typed report containing
  feasibility, boundary outcome and source correspondence

#### Scenario: Binding cancellation is atomic

- **WHEN** a host cancels a partial-retopology call
- **THEN** it SHALL receive the cancellation error and the input mesh SHALL be
  unchanged
