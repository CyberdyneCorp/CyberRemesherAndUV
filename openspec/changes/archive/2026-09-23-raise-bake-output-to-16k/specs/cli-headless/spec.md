## ADDED Requirements

### Requirement: A bounded bake working set from the CLI

The CLI SHALL accept `--bake-working-set <texels>` on a preset run, passing it to the export
bundle as its working-set bound. Zero (the default) SHALL mean no bound; a negative or
non-numeric value SHALL be an argument error. The JSON report SHALL record, for every map
file, its region count, region height, halo and the working set held.

#### Scenario: A preset export under a working-set bound
- **WHEN** `cyber` runs a preset export with `--bake-working-set` set far below one map's texel count
- **THEN** the export SHALL succeed, the map files SHALL equal those of the same run without the flag, and the report SHALL list more than one region per map
