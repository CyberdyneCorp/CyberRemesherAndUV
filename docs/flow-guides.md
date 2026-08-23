# Flow guides and painted density

Two explicit, opt-in local controls on the remeshing pipeline
(`openspec/specs/remeshing-pipeline`, change `add-flow-guides`):

- **Flow guides** — 3D polylines drawn on or near the Target. Near a guide the
  cross field is biased toward the guide's tangent, so extracted edge loops
  follow the stroke.
- **Painted density** — a per-vertex or per-face scalar on the Target that
  multiplies local target sizing, so a painted region gets smaller quads.

Neither is inferred. This is deliberately *not* the automatic curvature-driven
adaptivity that the roadmap descoped after its claimed win was retracted by
measurement: both inputs come from the user, and both default to absent.

**With no guidance supplied, output is byte-identical to a run without the
guidance arguments at all.** That is structural, not tuned: every guidance code
path sits behind a null/empty check, so an unguided run executes textually the
same arithmetic it did before this feature existed.

(Caveat on verification, not on the guarantee: on a build with
`-DCYBER_WITH_QUADCOVER=ON`, smooth organic meshes route to the vendored
Geogram solve, which is already run-to-run non-deterministic on stock `main`.
Byte-identity was therefore checked against `fandisk` — deterministic and
byte-for-byte unchanged — plus `ctest -R bench` and in-process bit
comparisons, not against `spot` or `stanford-bunny`.)

**A density of 1.0 everywhere is dropped, not applied.** 1.0 is the identity of
the sizing relation below, so a uniform-1.0 paint asks for nothing — but merely
*carrying* a density field forces the native seamless route (see below), which
would change the mesh. `validateGuidance` therefore drops a post-clamp all-1.0
density and reports it (`density is 1.0 everywhere (no sizing change);
ignored`), so `--guides` with a neutral density is byte-identical to no
`--guides` at all. A uniform density of any *other* value is a real request and
is applied normally.

## The sizing relation

Density is a **quads-per-unit-area multiplier**. A quad covers `edge^2` of
area, so

```
localEdgeLength = baseEdgeLength / sqrt(density)
```

Painting `4.0` asks for 4x the quads there, i.e. half the edge length. Because
it is area-relative, density composes with the area-derived `targetQuadCount`
instead of fighting it.

## Ranges and defaults

| Parameter | Range | Default | Out of range |
|---|---|---|---|
| guide `strength` | `[0.0, 1.0]` | `1.0` | clamped, reported as a warning |
| guide `radius` | `> 0`, world units | none (required) | **fatal** — a zero-radius guide could never be honored |
| guide `points` | at least 2 | none (required) | **fatal** |
| density value | `[0.25, 4.0]` | `1.0` | clamped, reported as one aggregated warning |
| density array length | exactly the Target's live vertex **or** face count | — | **fatal** |

Non-finite coordinates, strengths, radii or density values are fatal. Supplying
both a per-vertex and a per-face density array is fatal.

Every clamp and every rejection is reported: through `PipelineResult::parameterIssues`
in C++, the `CyberWarningCb` in the C ABI, `Mesh.guidance_warnings` plus
`warnings.warn` in Python, and the `guidance` block of the CLI `--report` JSON.

## Which backend honors what

Guidance is offered to each island's quadrangulator through
`IQuadrangulator::acceptGuidance`, whose **default implementation declines**.
A backend that declines is named in the per-island report — guidance is never
silently dropped.

| Backend | Flow guides | Painted density |
|---|---|---|
| quad-cover, native seamless solve | yes (cross field) | yes (isotropic pre-remesh **and** per-face grid spacing) |
| quad-cover, vendored Geogram solve | no — reported | no — reported |
| field-aligned | yes (cross field) | yes (via the pipeline's isotropic stage) |
| instant-meshes | no — reported | no — reported |
| integer | no — reported | no — reported |
| greedy | no — reported | no — reported |

### The forced native route (a real quality trade)

Supplying guidance to the quad-cover backend **forces the native seamless
solve**. The vendored Geogram `quad_cover` builds its own frame field from its
own single scalar density inside sources we do not patch, so it has no hook for
either input; routing a guided island there would silently drop the guidance,
which is precisely the failure this feature exists to avoid.

The one exception is the neutral paint above: a density of 1.0 everywhere is
dropped at validation, so it never reaches this decision and never moves the
route.

Setting the developer kill switch `CYBER_QC_NO_NATIVE` disables the native solve
outright, so a guided island routes to the vendored solve (or fails, on a build
without it). That case is **reported, not silently dropped**: the island's
`guidance_warnings` entry names the env var and says the vendored solve has no
guide/density hook, and `guidesHonored` is false. The guidance is still lost —
unset the variable to get it applied — but the run says so.

On builds with `-DCYBER_WITH_QUADCOVER=ON`, smooth organic meshes normally route
to the vendored solve (1-4% irregular) while native sits at ~4-5%. **A guided
organic mesh will therefore measure worse on irregular-vertex count than the
same mesh unguided.** That is unavoidable, not a bug, and it is stated here
rather than hidden. If native declines the island and the vendored path runs
anyway, the island report says so.

## Soft, not hard

Guides are a **soft** constraint. Hard pins — feature edges, boundaries and
crease alignment — still win, because promoting guides to hard constraints
re-opens the singularity campaign. A guide drawn across a tagged crease is
therefore only partially honored; the conflicted-face count is reported
(`CrossField::guideConflictFaces`) rather than the guide being silently applied
or silently dropped.

Note that on a **flat** panel the crease pins flood-fill across the whole
coplanar patch, so a guide crossing a crease on flat CAD geometry can be
entirely outvoted. The count tells you.

When the pins take **every** face a guide reached (`guidedFaces == 0` with a
non-zero conflict count) the field is exactly the unguided field, so the island
is reported as **not honored** — a `guidance_warnings` entry naming the number of
absorbed faces — rather than counted as honored because the backend accepted the
input. A partial absorption (some faces biased, some pinned) still counts as
honored; the guide did move the field.

## Limitations

- **Influence is Euclidean, not geodesic.** A normal-consistency gate stops a
  guide bleeding to geometry whose normal opposes it (the two sides of a thin
  limb), but a *tight fold* whose normals agree can still be reached. A
  heat-method or dual-graph geodesic radius is the follow-up.
- Guide sampling is O(faces x guide segments) with an AABB early reject. Fine
  for a few hundred segments on a 50k-face solve; a pathological DCC stroke with
  thousands of segments would be noticeable. Resample by arc length if it shows up.
- The `GuidanceField` samples the ORIGINAL input geometry. It survives
  triangulate / weld / orient / island split / isotropic remesh because it
  answers geometric queries, not indexed ones — but it also means guidance is
  authored against the input, never against an intermediate.

## Measured exit gate (honest result)

The proposal's exit gate is **<= 15 degrees mean deviation between extracted
loop direction and the guide tangent inside the influence radius** (folded mod
90, so a random field averages 22.5).

Measured by `examples/17_flow_guides.py` on the real corpus at 2000 quads,
default (quad-cover) backend:

| Model | radius | unguided | guided | gate |
|---|---|---|---|---|
| spot | 0.03 x diag | 30.01 | **18.17** | not met |
| stanford-bunny | 0.03 x diag | 15.55 | **13.15** | met |
| spot | 0.06 x diag | 26.74 | **25.10** | not met |
| stanford-bunny | 0.06 x diag | 15.40 | **11.31** | met |
| spot | 0.12 x diag | 24.08 | **19.69** | not met |
| stanford-bunny | 0.12 x diag | 16.06 | **15.63** | not met |

Mean guided deviation **17.17 deg**, worst **25.10 deg**.
**The gate is NOT met corpus-wide (2 of 6 runs at or under 15 degrees).**

Guides improve the alignment on **every** run measured, and the constraint is
demonstrably strong at the field stage — isolated on a flat grid with a
45-degree guide, the in-radius cross field goes from 44.84 to 11.29 degrees off
the tangent (`tests/quadrangulate/test_flow_guides.cpp`), and end to end on a
tube through the field-aligned backend from 12.08 to 9.23 degrees
(`tests/core/test_flow_guides_pipeline.cpp`). The loss happens between the
field and the extracted mesh: a soft bias is diluted by the converged transport
smoothing, then by the seamless Poisson solve, then by integer rounding and
isoline extraction. Closing the remaining gap means carrying the guide into the
seamless solve as a per-face target-frame rotation rather than only as a field
seed — that is follow-up work, not part of this change.

## Reaching it

### CLI sidecar

```
cyberremesh --input model.obj --output out.obj --guides guides.json --report run.json
```

```json
{
  "version": 1,
  "guides": [
    { "points": [[0,0,1],[0.5,0,0.7],[0.9,0,0.2]], "strength": 1.0, "radius": 0.15 }
  ],
  "density": { "perVertex": [1.0, 1.0, 4.0, "..."] }
}
```

Use `"perFace"` instead of `"perVertex"` for a per-face array. A missing,
unreadable or malformed sidecar, an unknown `version`, or any fatal validation
issue is **exit 2** naming the file and the offending guide index — never a
silent skip. The `--report` JSON gains a `guidance` block carrying the
POST-CLAMP per-guide values, the density clamp range, the validation issues and
one row per island (`guidesHonored`, `densityHonored`, `reason`).

### C ABI

`cyber_remesh` and `CyberRemeshParams` are unchanged and ABI-identical. Guided
runs use the new entry point:

```c
CyberFlowGuide guide = { points, point_count, /*strength=*/1.0f, /*radius=*/0.15f };
CyberGuidance  guidance = { &guide, 1, vertex_density, vertex_count, NULL, 0 };
cyber_remesh_guided(in, &params, &guidance, progress, cancel, warning, user, &out);
```

### Python

```python
from cyberremesh import FlowGuide, Mesh, RemeshParams, remesh

guide = FlowGuide(points=[(0, 0, 1), (0.5, 0, 0.7)], strength=1.0, radius=0.15)
result = remesh(mesh, RemeshParams(target_quad_count=5000), guides=[guide],
                density=per_vertex_values)
print(result.guidance_warnings)   # also raised through warnings.warn
```

### Network bridge

New commands (`kProtocolVersion` is **not** bumped — adding commands is
backward compatible, and bumping would reject every existing client):

| Command | Payload | Reply |
|---|---|---|
| `push_guides` | `{"guides":[{"points":[[x,y,z],...],"strength":1.0,"radius":0.15}]}` | `{"type":"ok","guides":N}` |
| `pull_guides` | — | `{"type":"guides","guides":[...]}` |
| `clear_guides` | — | `{"type":"ok"}` |
| `push_density` | `{"values":[...]}` | `{"type":"ok","values":N}` |
| `pull_density` | — | `{"type":"density","values":[...]}` |

Guidance is stored on the `BridgeSession` alongside the Target and is dropped by
`clear_scene` / `close_document`, since it is authored against that geometry.
The remesh itself is not a bridge command today, so the bridge's role here is
transport and storage.
