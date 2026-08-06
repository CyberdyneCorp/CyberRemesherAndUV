# Tasks: add-export-presets

- [x] 1. Preset schema (versioned) + resolver: map set, naming pattern,
       color-space table, normal green-channel, mesh format + flags
       — `cyber/core/export_preset.hpp` + `src/export_preset.cpp`. The preset is
       pure DATA: it lives in core and names map kinds through its own
       `PresetMap` enum rather than `bake::BakeMap`, so core keeps knowing
       nothing about baking. `resolvePreset` takes a built-in name first and a
       file path second, so a typo'd name reports "unknown preset (built-ins:
       ...)" instead of a confusing file-not-found. Rejection is loud on all
       four axes: unsupported `schemaVersion` (typed
       `ErrorCode::IncompatibleVersion`, naming both versions, checked BEFORE
       any other field), unknown fields, unknown map names, and a
       `namingPattern` missing `{map}` (which would have every map overwrite
       the previous one).
- [~] 2. Built-ins: blender, unity, unreal, gltf-generic (verify each in its
       target app: Principled BSDF hookup, Unity standard shader, Unreal
       material, glTF validator)
       — all four ship and their conventions are pinned by unit tests: only
       `unreal` is DirectX-green (-Y), only `gltf-generic` omits curvature
       (glTF 2.0 core has no slot), and color is the only sRGB map in every
       built-in. **The in-app verification is NOT done** and this task stays
       partial: Blender was not running (no MCP connection) and Unity/Unreal
       are not available in this environment. The conventions are encoded from
       each target's documented spec, not observed. The glTF output was checked
       structurally only (valid GLB magic/version, JSON+BIN chunk lengths
       consistent, glTF 2.0 asset) — no conformance validator was available.
- [x] 3. Export packaging path honoring a preset (mesh + maps + naming)
       — new `cyber_exportbundle` module (`src/exportbundle`), sitting above
       core and below the CLI because it needs bake + imageio + uv. It unwraps
       the low-poly when it has no UVs (otherwise `--preset` would be useless
       straight after a remesh) and writes the UVs back to the caller's mesh so
       the exported mesh and the maps agree on one layout. Conventions are
       applied at write time: DirectX presets flip the normal map's green
       channel, sRGB maps are encoded before the 8-bit write. sRGB on an EXR
       preset is reported as a warning and written linear rather than silently
       applied or silently dropped. Rides on `CYBER_BUILD_UV`.
- [x] 4. CLI `--preset`, report coverage, error path for unknown/incompatible
       — plus `--list-presets`, `--texture-size`, `--ao-samples` and `--cage`.
       The preset resolves BEFORE the remesh, so an unknown name costs nothing.
       Report gains `preset` (name, schema version, resolved map set,
       conventions, whether it unwrapped) and `outputs` (every file with its
       written color space and size). Fixed alongside: `elapsedSeconds` stopped
       at the end of the solve and understated a preset run by the whole bake.
       A build without `CYBER_BUILD_UV` reports that it cannot honor presets
       rather than silently skipping them.
- [x] 5. Bindings reachability + tests (schema-version rejection, per-preset
       golden outputs)
       — 21 C++ cases (`tests/core/test_export_preset.cpp`,
       `tests/exportbundle/test_bundle.cpp`) and 17 CLI checks
       (`tests/cli/test_cli.py`). Covered: built-in conventions per preset,
       version rejection naming both versions and beating every other field to
       the error, unknown-field/unknown-map/bad-pattern rejection, naming-token
       expansion and per-preset filename uniqueness, the sRGB curve at its
       anchors, and end to end — mesh + exactly the preset's maps, auto-unwrap,
       the DirectX green flip measured as `gl + dx == 255`, sRGB-on-EXR warning,
       mesh-format mismatch warning, cancellation, a user preset file honored
       like a built-in, and a future-schema preset producing NO partial output.
       Not done: no C ABI / Python surface for presets — the C ABI has no export
       entry point to hang them off, and adding one is `engine-bindings` work,
       not this change. Golden outputs were not recorded: the maps are 2048²
       by default and the run is dominated by a non-deterministic-cost AO bake,
       so the tests assert conventions and structure instead.
- [x] 6. Docs + CHANGELOG
       — README gains a "Per-DCC export presets" section with the schema, the
       built-in differences and the measured cost; CHANGELOG under
       `## Unreleased`, including the two known limitations.

## Follow-ups this change surfaced

- **The AO bake is ~96% of a preset run** (measured on spot at 512²: 25.5s with
  AO, 0.96s without) and scales with texel count, so the default 2048² set
  takes minutes. `bake()`'s texel loop is serial and embarrassingly parallel;
  `IBackend::parallelFor` already exists. This is the single highest-value
  follow-up and belongs in its own change, with the usual byte-identical-output
  gate.
- **glb presets do not reference their maps from the glTF material.** Making
  `unity`/`unreal`/`gltf-generic` truly one-click means writing a material with
  texture references, which is `mesh-io` glTF-export work.
