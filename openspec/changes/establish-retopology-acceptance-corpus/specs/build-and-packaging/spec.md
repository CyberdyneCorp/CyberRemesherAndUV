## MODIFIED Requirements

### Requirement: Test gates
CI SHALL gate every merge on: unit tests (core modules), mesh-kernel property tests, stroke-recognizer trace tests, backend parity tests (on Metal and CUDA hardware lanes), and the golden-mesh regression suite (recorded baselines for quad count, non-quad count, singularity count, Hausdorff distance, and topology-validity metrics on a versioned permissively-licensed offline corpus). The quality-gate lane SHALL fail, rather than pass or skip, when its required corpus fixtures, baseline identity, solver identity, or metric dependencies are unavailable.

The **release** lane SHALL be gated too: no artifact SHALL be published from a tree whose suite has not been run in the release job itself, and the platform legs SHALL test the configuration they ship — including the field solver, so a release cannot silently carry a different quadrangulator than the one CI measured. A scheduled hardening lane SHALL re-run the suite under AddressSanitizer/UndefinedBehaviorSanitizer (in each quadrangulator configuration the project ships) and ThreadSanitizer, and SHALL run the fuzz harnesses; the fuzz harnesses and a seed corpus SHALL be checked in, and replaying that corpus SHALL run as an ordinary test case on every CI leg so the harnesses cannot rot between scheduled runs.

#### Scenario: Regression drift fails CI
- **WHEN** a change alters a golden mesh's quad count beyond tolerance
- **THEN** CI SHALL fail showing baseline vs. observed metrics

#### Scenario: Acceptance gate lacks required evidence
- **WHEN** the quality-gate job cannot load its required acceptance fixture,
  compatible baseline, solver identity, or metric dependency
- **THEN** CI SHALL fail with the missing evidence named rather than treating
  the condition as a successful skip

#### Scenario: Untested artifacts are never published
- **WHEN** the release workflow runs
- **THEN** the packaging jobs SHALL depend on a job that runs the test suite, and a failing suite SHALL block every artifact

#### Scenario: Sanitizer and fuzz coverage is scheduled, and its inputs are kept
- **WHEN** the scheduled hardening lane runs
- **THEN** it SHALL execute the suite under ASan/UBSan and TSan and run the fuzz targets, and the seed corpus checked in alongside them SHALL replay as a test case on ordinary CI legs
