## ADDED Requirements

### Requirement: Guarded half-lattice component quantizer

The seamless-UV pipeline SHALL provide an opt-in half-lattice component
quantizer that consumes the topology layout and Bi-MDF targets. It SHALL retain
the current guided/greedy quantizer as the fallback for every component it does
not fully realize. The component quantizer SHALL remain non-default until the
benchmark validity, layout, and quality gates are met.

#### Scenario: Safe mixed solve

- **WHEN** an island contains both an eligible half-lattice component and a
  rejected component
- **THEN** the pipeline SHALL use the component assignment only for the
  eligible component
- **AND** it SHALL use the fallback quantizer for the entire rejected component
- **AND** the produced mesh SHALL satisfy the normal layout and mesh-validity
  gates

#### Scenario: Unsafe component solve

- **WHEN** applying a purported component assignment fails a layout invariant,
  mesh-validity check, or target-residual check
- **THEN** the pipeline SHALL discard the component assignments for that island
- **AND** rerun the existing fallback quantizer
- **AND** the report SHALL name the failed gate
