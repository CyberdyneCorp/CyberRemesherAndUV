# cli-headless — sculpt handoff input

Delta for `add-claycore-bridge`.

## ADDED Requirements

### Requirement: Handoff-driven pipeline from the CLI
The CLI SHALL accept a sculpt handoff (file path or stdin) as its Target
input and run remesh → unwrap → bake in one invocation against it. The
machine-readable report SHALL record the handoff version and, when a bake
used a field evaluator, which sampled maps came from the field.

#### Scenario: One-command sculpt-to-asset
- **WHEN** `cyber` runs with a handoff Target, a remesh preset, and bake map selection
- **THEN** it SHALL produce the low-poly mesh and requested maps in one invocation and the report SHALL record the handoff version
