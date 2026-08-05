# Tasks: add-export-presets

- [ ] 1. Preset schema (versioned) + resolver: map set, naming pattern,
       color-space table, normal green-channel, mesh format + flags
- [ ] 2. Built-ins: blender, unity, unreal, gltf-generic (verify each in its
       target app: Principled BSDF hookup, Unity standard shader, Unreal
       material, glTF validator)
- [ ] 3. Export packaging path honoring a preset (mesh + maps + naming)
- [ ] 4. CLI `--preset`, report coverage, error path for unknown/incompatible
- [ ] 5. Bindings reachability + tests (schema-version rejection, per-preset
       golden outputs)
- [ ] 6. Docs + CHANGELOG
