## ADDED Requirements

### Requirement: Current integration contract

The project SHALL publish one current document that distinguishes supported,
experimental, scaffolded and build-dependent integration paths, and SHALL link
to it from the root documentation. Historical roadmaps and benchmark records
SHALL be labelled as dated evidence rather than current guarantees.

#### Scenario: Mobile consumer evaluates support

- **WHEN** an iOS or Android consumer evaluates the library
- **THEN** the documentation SHALL state whether the path has device evidence,
  cross-compilation evidence only, or no published SDK artifact

#### Scenario: ABI consumer evaluates reproducibility

- **WHEN** a host integrates through the C ABI
- **THEN** the documentation SHALL distinguish ABI compatibility from engine and
  solver identity required for reproducible output
