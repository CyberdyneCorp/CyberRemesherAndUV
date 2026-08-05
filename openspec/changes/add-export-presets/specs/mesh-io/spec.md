# mesh-io — per-DCC export presets

Delta for `add-export-presets`.

## ADDED Requirements

### Requirement: Named export presets
Export SHALL support named presets that bundle: the set of bake maps to emit,
the output file naming pattern, color-space conventions per map (sRGB vs
linear), the normal-map green-channel convention (+Y or −Y), the mesh format
and its per-format flags, and unit/axis metadata where the chosen format
carries it. The engine SHALL ship built-in presets `blender`, `unity`,
`unreal`, and `gltf-generic`, and SHALL load user-defined preset files using
the same schema.

#### Scenario: One preset produces a ready-to-import set
- **WHEN** an export runs with the `blender` preset on a baked document
- **THEN** the output SHALL contain the mesh and exactly the preset's map set, named per its pattern, with the normal map in the preset's green-channel convention

#### Scenario: User preset behaves like a built-in
- **WHEN** a user preset file with a valid schema is passed by path
- **THEN** the export SHALL honor it exactly as it would a built-in preset

### Requirement: Preset schema is versioned and fails loudly
Preset files SHALL declare a schema version. Loading a preset with an
incompatible schema version SHALL fail with a typed error naming the found and
supported versions, and SHALL NOT silently ignore unknown fields that change
output content.

#### Scenario: Incompatible preset is rejected loudly
- **WHEN** a preset file declares a schema version the engine does not support
- **THEN** the export SHALL fail with a typed error identifying both versions, producing no partial output
