# cyberremesher roadmap

Where the engine is, what it is missing, and in what order the gaps are worth
closing. Derived from the 3DCoat feature study (`~/work/3dcoat_study/`), reduced
to **what this repository owns** — sculpt-side items live in ClayCore, app-side
items in ClaySpace, and they are named here only where this engine has to
provide something for them.

Living requirements are in `openspec/specs/`; this file is the plan, not the
contract. A row becomes real when it becomes a change in `openspec/changes/`.

## Where the engine is (2026-08-05, v0.5.0)

14 capabilities, 5 archived changes, one change complete and awaiting archive
(`bimdf-quantization`, 5/5 tasks — the singularity campaign closed its last
gate: multires cross field is the default and nefertiti pure singularities meet
the ≤200 gate on the stock path).

- Auto quad remeshing: quad-cover default beating QuadriFlow on 3/5 corpus
  models, crease-fraction routing, pure-quad path, open-surface hole fill
- UV atlas: chart-grow → merge → LSCM → re-orient → skyline pack; ~2× lower
  conformal distortion than xatlas (xatlas still packs tighter)
- Manual retopology: 30+ operators + stroke-gesture recognition + snapping in
  the C ABI; the interactive tool UX is spec'd but largely unbuilt
- Baking: normal / AO / displacement / color with an editable cage
- CPU/Metal/CUDA/OpenCL compute; Metal|Vulkan viewport; desktop, iPad, Android,
  headless CLI, network bridge shells

## The gap, in one sentence

The engine remeshes, unwraps and bakes at measured state-of-the-art quality,
but nothing connects it to the sculpt engine next door, and the guidance and
interaction affordances that make 3DCoat's retopo/UV rooms *usable by artists*
— flow guides, soft selection, routed seams, curvature maps, per-DCC exports —
are absent from the specs entirely.

## Phase 1 — complete the standard exit recipe

Small, independent, immediately useful: the practitioner corpus bakes
curvature + AO + normal at 4K with a per-target preset before any texturing.
We bake three of the four and have no presets.

| Change | Why |
|---|---|
| `add-curvature-bake` | Curvature/cavity joins normal/AO/displacement/color under the same cage and tangent rules. The missing quarter of the standard recipe. |
| `add-export-presets` | Named per-DCC bundles (Blender, Unity, Unreal, glTF-generic): map set, naming, color space, mesh-format flags; `--preset` in the CLI and echoed in the JSON report. Small work, big pipeline feel. |

## Phase 2 — guidance and interaction

| Change | Why |
|---|---|
| `add-flow-guides` | User strokes as cross-field alignment constraints and painted density as sizing multipliers — 3DCoat AUTOPO's control surface, honored more principledly by a field-based pipeline. Distinct from the descoped *automatic* adaptivity: this is user-controlled. |
| `add-soft-selection` | Gradient region selection (line with angle snap / sphere / painted) + weighted transform + auto re-snap to the Target. 3DCoat 2022 parity; mostly composition of existing operators. |
| `add-seam-path-tool` | Auto-routed point-to-point seams over `shortest_vertex_path` with feature-biased costs — the tool built for exactly the spiral-looped meshes auto-retopo produces. Complements, not replaces, the spec'd stroke-over-edges seams. |

## Phase 3 — the pipeline

| Change | Why |
|---|---|
| `add-claycore-bridge` | The product story: ClayCore sculpt → Target handoff → remesh → UV → bake, with an optional field-evaluation interface so bakes sample exact normals/AO from the SDF instead of raycasting a mesh, and a `conform` operation that re-snaps an EditMesh when the sculpt changes. ClayCore's roadmap (Phase 3 there) provides the other half; neither engine has the seam today. Zero hard dependency on ClayCore — everything arrives through a versioned interchange and an evaluator interface. |

## Standing implementation debt (spec'd, unbuilt)

Not spec gaps — listed so the plan is honest about where the effort goes after
Phase 1: the interactive retopo tool gallery and the UV editor UX
(`manual-retopology`, `uv-editing`, `application-shell`) are written in detail
and the shells realize little of them. The engine outclasses the app.

## Deliberately not doing

- **Automatic adaptivity** (Phase 2 of the quality campaign) stays descoped —
  the measurements retracted its claimed win. Painted density (user-controlled)
  is the replacement, in `add-flow-guides`.
- **Subdivision-level multires on EditMesh** (retopo mesh as sculpt base) —
  3DCoat parity item with no current consumer; revisit after the bridge ships.
- **Additional unwrap algorithms (ABF, …)** — LSCM already beats the xatlas
  baseline on distortion; the remaining gap is packing, tracked in the quality
  campaign, not a new unwrapper.
- **Volumetric/SDF representation** — ClayCore owns volumes; this engine stays
  a pure surface-mesh library.

## Requirements taken from their bugs

Worth writing into the specs they touch, because a competitor's known failure
is a free test case:

- guidance inputs are honored loudly or rejected loudly — never silently
  ignored (their AUTOPO guides fail quietly when symmetry modes conflict)
- seam-path points remain editable until commit, and a dropped resume point
  never discards committed seams
- conform preserves topology and reports maximum deviation rather than
  silently stretching
- export presets are versioned data, and preset schemas survive engine
  versioning (their brush-preset massacre, applied to our exports)
- soft-selection edits keep the mesh glued to the Target — auto-snap is part
  of the operation, not a separate cleanup pass
