# cli-headless — export preset flag

Delta for `add-export-presets`.

## ADDED Requirements

### Requirement: Preset-driven export from the CLI
The CLI SHALL accept `--preset <name-or-path>` selecting a built-in or
user-defined export preset. The machine-readable report SHALL record the
effective preset (name, schema version, resolved map set) and every output
file produced. An unknown preset name SHALL exit with the documented
argument-error code and list the available built-ins.

#### Scenario: Preset export from the command line
- **WHEN** `cyber` runs with `--preset unreal` on an input that bakes successfully
- **THEN** the outputs SHALL match the unreal preset's bundle and the JSON report SHALL list the preset and each emitted file

#### Scenario: Unknown preset fails as an argument error
- **WHEN** `cyber` runs with `--preset nosuch`
- **THEN** it SHALL exit with the argument-error code and print the built-in preset names
