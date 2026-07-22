# engine-bindings (delta)

## ADDED Requirements

### Requirement: Automatic UV atlas binding
The C ABI and Python bindings SHALL expose the automatic UV atlas so a caller can, in one call, generate a packed UV atlas for a mesh and read back its quality. The C symbols SHALL always be declared (stable ABI) even in a build without the UV module, returning a runtime error there rather than being absent.

#### Scenario: One-call atlas from Python
- **WHEN** a caller invokes `Mesh.unwrap_atlas` (C: `cyber_uv_atlas`) on a loaded mesh
- **THEN** the binding SHALL seam, unwrap, re-orient, and pack the mesh in place and return an atlas result — chart count, seam-edge count, max/RMS conformal distortion, flipped- and fallback-chart counts, packed area, and texel density — and a subsequent OBJ save SHALL emit the per-corner UVs (`vt` / `f v/vt`)

#### Scenario: Atlas parameters mirror the engine options
- **WHEN** a caller supplies atlas parameters (C: `CyberAtlasParams` / Python: `AtlasParams`)
- **THEN** the binding SHALL honour the chart-angle bound, the chart-merge toggle and distortion cap, the re-orientation toggle, and the pack margin / texture size, defaulting them from the engine's own defaults via `cyber_default_atlas_params`

#### Scenario: ABI stable without the UV module
- **WHEN** the engine is built without the UV module
- **THEN** `cyber_uv_atlas` SHALL still be a declared, linkable symbol that returns a runtime error status (not a missing symbol), so binaries built against the header keep a stable ABI
