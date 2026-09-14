## Purpose

Safely transfers compatible topology-layout assignments to the final seamless
quantizer by solving complete doubled-lattice components instead of pinning an
unsafe subset of their variables.

## ADDED Requirements

### Requirement: Component-scoped layout realization

When the component solver is selected, the system SHALL partition the eligible
layout equations and reduced variables into deterministic connected components.
It SHALL either realize every variable and equation in a component through its
doubled-lattice solution or leave that entire component to the existing fallback
quantizer. It SHALL NOT inject only a clean subset of an otherwise rejected
component.

#### Scenario: Independent eligible component

- **WHEN** a layout component has a complete equation-to-variable relation and
  a valid doubled-lattice solution
- **THEN** the system SHALL apply that component's complete assignment before
  the fallback quantizer runs
- **AND** the report SHALL count the component and all of its realized arcs as
  injected

#### Scenario: Component connected to an unsupported relation

- **WHEN** any equation or variable in a component depends on a continuous,
  excluded, fractional, or otherwise unsupported relation
- **THEN** the system SHALL reject the complete component from injection
- **AND** the fallback quantizer SHALL retain ownership of every variable in
  that component

### Requirement: Exact doubled-lattice parity

The component solver SHALL represent half-step values on an integer doubled
lattice and SHALL enforce all component parity constraints before assigning any
final integer values. A component with contradictory parity constraints or an
assignment outside the existing integer bounds SHALL be rejected without
partially applying it.

#### Scenario: Parity-consistent component

- **WHEN** the doubled-lattice equations admit a parity-consistent bounded
  solution that realizes the Bi-MDF arc targets
- **THEN** the system SHALL convert the complete solution to the seamless
  quantizer's integer assignments without rounding an unresolved half-step

#### Scenario: Parity contradiction

- **WHEN** the doubled-lattice equations contain an inconsistent parity cycle
- **THEN** the system SHALL report the component as parity-inconsistent
- **AND** it SHALL not modify the final quantizer inputs for that component

### Requirement: Observable component injection outcome

The run report SHALL expose deterministic totals for components considered,
injected, and rejected by named reason, including unsupported relation, parity
inconsistency, bound violation, and target residual. It SHALL separately report
the count of arcs and variables actually injected by complete components.

#### Scenario: Report-only execution

- **WHEN** the component solver runs in report-only mode
- **THEN** the report SHALL include the same component classification as an
  injection-enabled run on the same input
- **AND** the output mesh SHALL match the fallback quantizer output byte for
  byte

#### Scenario: Reproducible component census

- **WHEN** the same input and options are solved twice
- **THEN** component counts, rejection-reason counts, and the list ordering in
  diagnostic export SHALL be identical
