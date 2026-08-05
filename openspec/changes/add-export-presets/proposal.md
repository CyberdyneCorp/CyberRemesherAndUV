# Proposal: per-DCC export presets

## Why

Every observed 3DCoat pipeline exit runs through a named per-target preset
("Blender", "Unreal 4", …) that bundles which maps to emit, how to name them,
and how the target app expects to read them. We export meshes and bake maps
with full manual control but no presets — so every export is a re-derivation
of the same decisions, and the CLI cannot express "give me a Blender-ready
folder" in one flag. Small work, large pipeline feel.

## What Changes

- **Named export presets** as versioned data: each preset bundles the map set
  (e.g. normal + AO + curvature + color), file naming pattern, color-space and
  normal-map conventions (±Y), mesh format and its flags, and texel/units
  metadata where the format carries it.
- Ship four built-ins — `blender`, `unity`, `unreal`, `gltf-generic` — and
  accept user-defined preset files with the same schema.
- CLI: `--preset <name>`; the JSON report records the effective preset and
  every file it produced.
- Presets are **versioned**: a preset file declares its schema version and the
  engine refuses loudly on an incompatible one (the anti-3DCoat requirement —
  their engine rewrite silently destroyed user preset libraries).

## Capabilities

### Modified Capabilities

- `mesh-io`: export presets join the export surface.
- `cli-headless`: `--preset` and report coverage.

## Impact

- `src/io` (preset schema + resolver), bake output packaging, CLI flags,
  bindings, docs. Additive.
- Non-goals: AppLink-style live bridges into DCCs (the network bridge already
  covers live interchange); import presets.
