## MODIFIED Requirements

### Requirement: Named export presets
Export SHALL support named presets that bundle: the set of bake maps to emit,
the output file naming pattern, color-space conventions per map (sRGB vs
linear), the normal-map green-channel convention (+Y or −Y), the mesh format
and its per-format flags, and unit/axis metadata where the chosen format
carries it. The engine SHALL ship built-in presets `blender`, `unity`,
`unreal`, and `gltf-generic`, and SHALL load user-defined preset files using
the same schema.

**The naming pattern SHALL carry the UDIM TILE NUMBER.** A `{udim}` token SHALL expand to
the tile number of the map being written, under the `1001 + u + 10*v` numbering
`surface-baking` uses. An export that is not UDIM-aware SHALL expand it to `1001`, because
the unit square is that tile — the token therefore means the same thing in both, and a
preset does not have to be rewritten to be used for either. A UDIM export writing MORE THAN
ONE TILE through a pattern that does NOT contain the token SHALL be REFUSED with a
diagnostic naming the pattern, rather than writing every tile to one path where each
overwrites the last while the report lists them all.

#### Scenario: One preset produces a ready-to-import set
- **WHEN** an export runs with the `blender` preset on a baked document
- **THEN** the output SHALL contain the mesh and exactly the preset's map set, named per its pattern, with the normal map in the preset's green-channel convention

#### Scenario: User preset behaves like a built-in
- **WHEN** a user preset file with a valid schema is passed by path
- **THEN** the export SHALL honor it exactly as it would a built-in preset

#### Scenario: The tile token expands to the tile being written
- **WHEN** a UDIM export runs with a naming pattern containing the tile token over a layout
  occupying more than one tile
- **THEN** each map SHALL be written once per occupied tile, to a path carrying that tile's
  own number

#### Scenario: A non-UDIM export expands the tile token to the unit square's tile
- **WHEN** an export that is not UDIM-aware runs with a naming pattern containing the tile
  token
- **THEN** the token SHALL expand to `1001`

#### Scenario: A multi-tile export through a pattern without the token is refused
- **WHEN** a UDIM export occupying more than one tile runs with a naming pattern that does
  not contain the tile token
- **THEN** the export SHALL fail with a diagnostic naming the pattern, and SHALL NOT write
  one tile over another
