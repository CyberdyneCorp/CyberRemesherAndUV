## ADDED Requirements

### Requirement: Observable bounded target-count calibration

The remesher SHALL report requested count, effective base count, every retained
calibration attempt, calibrated count, final output count, and a termination
reason for the whole run and every island. The default policy SHALL retain its
current behavior. An opt-in policy SHALL bound its number of attempts and SHALL
not assume count is monotone in spacing.

#### Scenario: Non-monotone correction

- **WHEN** a later calibration attempt is farther from the effective target
- **THEN** the report SHALL identify the closer retained incumbent rather than
  reporting the last attempt as selected

#### Scenario: Tolerance miss

- **WHEN** a bounded policy exhausts attempts without meeting its tolerance
- **THEN** the run SHALL report `tolerance-not-met` distinctly from cancellation
  and error, and SHALL return the best eligible incumbent

### Requirement: Validity dominates count proximity

A count candidate that violates required geometry, border, or topology validity
SHALL NOT displace an eligible incumbent solely because its count is closer.

#### Scenario: Invalid closer candidate

- **WHEN** a closer count candidate fails a required validity check
- **THEN** the report SHALL retain the valid candidate and name the rejection
