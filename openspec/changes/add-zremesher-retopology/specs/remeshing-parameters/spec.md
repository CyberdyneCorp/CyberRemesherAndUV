# remeshing-parameters — ZRemesher parameters

## ADDED Requirements

### Requirement: ZRemesher parameters are canonical and validated

The canonical parameter set SHALL gain the ZRemesher controls — quality mode
(fast / balanced / best), adaptive sizing toggle and weights, local-feature-size
preservation toggle, symmetry axis and mode, and the default guide mode — and
each SHALL be validated at every entry point with the same clamp-and-report
discipline as the existing parameters. No ZRemesher parameter SHALL be inert:
each SHALL demonstrably change the output or be rejected.

#### Scenario: Out-of-range ZRemesher parameter is clamped and reported

- **WHEN** a caller supplies a quality mode, symmetry axis or sizing weight
  outside its documented domain
- **THEN** the value SHALL be clamped or rejected, and the adjustment SHALL
  appear in the run report's warnings

#### Scenario: Defaults preserve shipped behavior

- **WHEN** every ZRemesher parameter is left at its default
- **THEN** the output SHALL be byte-identical to the shipped default pipeline
