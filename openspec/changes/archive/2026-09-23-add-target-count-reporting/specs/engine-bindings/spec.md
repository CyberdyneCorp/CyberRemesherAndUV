## ADDED Requirements

### Requirement: Count outcomes are available to embedded hosts

The C ABI and supported language bindings SHALL expose an additive count policy
and count outcome report so a host can decide whether to retry without parsing
diagnostic text. Existing parameter/report layouts SHALL remain unchanged.

#### Scenario: ABI-compatible count policy

- **WHEN** a host compiled against the existing remesh parameters invokes the
  existing entry point
- **THEN** it SHALL remain binary compatible and receive existing behavior
