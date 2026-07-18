# cli-headless — Headless Batch Remeshing

## ADDED Requirements

### Requirement: Dedicated headless binary
The CLI SHALL be a separate binary linking only the core engine and acceleration layers (no windowing, no GUI toolkit), suitable for servers and CI. It SHALL accept input/output paths, every canonical remeshing parameter, backend selection, and report options.

#### Scenario: Runs without a display
- **WHEN** the CLI runs on a headless Linux server
- **THEN** the remesh SHALL complete without requiring any display or GUI library

### Requirement: Validated arguments
All arguments SHALL be validated before work starts: unknown flags, missing required values, non-numeric numerics, and unreadable inputs SHALL fail fast with a specific message and exit code 2; out-of-range parameter values SHALL clamp with a printed warning per remeshing-parameters.

#### Scenario: Bad flag value
- **WHEN** the CLI is invoked with `--target-quads abc`
- **THEN** it SHALL print an error naming the flag and value and exit with code 2, running nothing

### Requirement: Exit codes reflect outcomes
The CLI SHALL exit 0 only on full success. Documented nonzero codes SHALL distinguish at minimum: argument errors (2), input load failure (3), pipeline failure or empty result (4), partial success with failed islands (5), output write failure (6), cancellation (130).

#### Scenario: Missing input file
- **WHEN** `--input` names a nonexistent file
- **THEN** the CLI SHALL print the load error to stderr and exit with code 3 (AutoRemesher exited 0 here)

#### Scenario: Unwritable output
- **WHEN** the output path cannot be written
- **THEN** the CLI SHALL exit with code 6 and name the path

### Requirement: Machine-readable reports
With `--report <path.json>` the CLI SHALL write a JSON report containing: tool version, input/output paths, effective (post-clamp) parameters, backend used, per-stage timings, quad/non-quad/vertex/singularity counts, per-island diagnostics including failures, and warnings. A human-readable summary SHALL always print to stdout. An unwritable report path SHALL be an error (exit 6), not silently skipped.

#### Scenario: Report content
- **WHEN** a run completes with one failed island
- **THEN** the JSON report SHALL list that island's failure stage and reason and the run status "partial"

### Requirement: Version and quiet output
`--version` SHALL print the real semantic version and build info. Default output SHALL be the summary only; `--verbose` gates diagnostic detail; `--quiet` suppresses everything but errors. Progress SHALL print as a single updating line only when stdout is a TTY.

#### Scenario: Version prints
- **WHEN** `--version` is invoked
- **THEN** a non-empty semantic version SHALL print (AutoRemesher printed an empty string)
