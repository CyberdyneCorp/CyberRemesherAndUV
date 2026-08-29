# ZRemesher-class automatic retopology — implementation plan

Living companion to the OpenSpec change `add-zremesher-retopology`. The change
holds the requirements and the task list; this file holds the *engineering*
rationale — why the pieces are shaped the way they are, and what each phase is
actually allowed to assume about the phases before it.

## The gap

The engine already owns nearly all the low-level machinery: 4-RoSy cross fields
with transport-aware smoothing, crease and boundary alignment, flow guides as
soft directional constraints, painted density, a native seamless
parameterization, QuadCover isoline extraction, a multiresolution
position-field path, pure-quad post-processing, motorcycle/T-mesh tracing, and a
Bi-MDF global integer quantizer over an in-tree min-cost flow.

What it does not own is **intentional global topology layout**. Today the system
answers:

> Given a directional field and an integer grid, generate valid field-aligned
> quads.

An artist-facing retopologizer has to answer a different question:

> What edge-loop and patch layout should this shape have, where do the
> extraordinary vertices go, where do loops start, terminate and merge, and how
> do artist hints change that layout?

The topology is currently *emergent*. Every improvement that needs to reason
about loops — singularity placement, topology guides, symmetry, semantic groups,
patch-grid extraction, quality scoring, debug rendering — would otherwise add
another conditional inside `bimdf_quantize.cpp`, which already owns tracing,
T-mesh construction, symbolic lengths, patch validation, parity, quantization,
injection and diagnostics at once.

The architectural rule that follows: whenever a future feature asks *"how do we
tell the automatic remesher where topology should go?"*, the answer is
**express it as a `ConstraintField` / `SizingField` input, or as a
`TopologyLayout` constraint** — never as another ad-hoc branch in the final quad
cleanup.

## The two views of one graph

`TopologyLayout` did **not** arrive by moving 3000 lines of tracing out of
`bimdf_quantize.cpp`. The tracer is where the layout is genuinely discovered —
separatrix walks, crease chains, boundary chains, orbit extraction — and it is
inseparable from the seamless solver's symbolic machinery. Moving it wholesale
would have been a large, risky diff that bought nothing.

The split is by **responsibility** instead, and the same graph now has two views
that share node and arc ids:

| | `bimdf::TMesh` | `cyber::remesh::TopologyLayout` |
|---|---|---|
| Role | quantization view | geometric / combinatorial view |
| Carries | arc lengths, each arc's exact symbolic length as a linear expression over the solver's promoted variables `z = [u \| v \| t \| c]`, patch sides | node positions and kinds, arc polylines, patch sides, quantized lengths |
| Consumed by | `solveBimdf` and the back-substitution, nothing else | singularity scoring, guides, symmetry, semantic groups, quality scoring, export, benchmarking |
| Depends on | the seamless solver's variable numbering | nothing outside the mesh |

A stage that needs both can hold both. A stage that only needs the topology —
which is nearly all of them — never sees a solver variable.

### Geometry capture is write-only

`Charts::captureGeometry` makes the tracer keep what it used to discard:

* **T-node positions** at node creation, by barycentric interpolation of the
  crossing UV inside its face. Weights are clamped into the triangle so a folded
  or UV-degenerate chart yields a point *on* the face rather than an
  extrapolation off the surface.
* **Separatrix polylines**, sliced out of the walk's trail. `flushSeg` already
  records every face crossing in the same monotone `curve` parameter the ray's
  events carry, so an arc — a curve interval between two events — slices
  directly out of it. The trail is cleared exactly where `ray.events` is
  cleared, so a jittered retry cannot leave stale geometry behind.
* **Crease and boundary polylines**, from the chain's own vertices between the
  two bounding events.

Nothing in `solveBimdf` reads any of it. That is the load-bearing property:
capture cannot move an assignment, which is why the Phase A gate — byte-identical
output with capture on versus off — is meaningful rather than accidental. It is
verified on all six corpus models.

### Validation has two failure classes

`validateTopologyLayout` deliberately does **not** collapse everything into one
boolean, because the tracer already distinguishes these and the layout must not
throw that distinction away:

* **Hard** — a bad id, a non-finite position, an arc pointing at a missing node,
  a sample face out of range, an arc bounding no patch while not marked
  excluded. The graph itself is corrupt; nothing may consume it. The first
  violation is named, in a fixed check order, so the report is deterministic.
* **Local** — a patch whose boundary walk does not close. The rest of the layout
  is sound. The patch is reported by id and its arcs are marked excluded, which
  is exactly how the tracer already contains a rejected orbit, and the sound
  remainder proceeds.

Collapsing the local case into a hard failure would throw away a whole model's
layout over one bad patch — on rocker-arm, one patch out of 199.

## Phases

| Phase | Content | Gate |
|---|---|---|
| **A** | `TopologyLayout`, validation, debug export | byte-identical Bi-MDF output; every currently successful layout validates |
| **B** | fold-robust tracing: orientation-independent contour following, combinatorial patch-sector classification, deterministic exact-vertex crossings | zero organic T-mesh fallbacks from folded patch sectors; negative-index cone fixture passes |
| **C** | `GeometryAnalysis`, weighted singularity cost, deterministic relocation, dipole cancellation | weighted cost improves; topological defects stay zero; geometry does not regress |
| **D** | unified `SizingField` fusing painted density, curvature, features and thickness | painted-density, target-count and thin-feature suites; byte-identical when disabled |
| **E** | topology-affecting flow guides (orientation vs topology mode, closed loop guides) | sphere-equator, eye-loop, mouth-loop adherence |
| **F** | `ConstraintField` semantic boundaries; exact forced-axis topology symmetry | mirrored *connectivity*, not merely mirrored positions |
| **G** | quality-driven candidate selection (fast / balanced / best) over one published score | `Best` never worse than either candidate, plus manual visual inspection |

Phase B is the largest single unlock for the existing advanced solver: the
relaxed map is not injective near high-distortion cones, and today a folded
patch sector is what makes an organic mesh fall back from the global quantizer
to per-translation greedy rounding.

## Current state

Phase A is complete and reachable behind `CYBER_ZR_LAYOUT`:

```sh
# report the layout statistics and the validation verdict
CYBER_QC_BIMDF=1 CYBER_ZR_LAYOUT=1 cyberremesh --input model.obj --output quads.obj \
    --quad-method quad-cover --target-quads 2000

# ...and write <prefix>.json (machine-readable) + <prefix>.obj (polylines)
CYBER_QC_BIMDF=1 CYBER_ZR_LAYOUT=/tmp/layout cyberremesh ...
```

`examples/21_topology_layout.py` runs that and renders the arcs and
singularities over the quads they produced.

Corpus layouts at 2000 target quads:

| model | nodes | arcs | patches | singularities | feature arcs | non-quad | non-closing | total index |
|---|---|---|---|---|---|---|---|---|
| cube | 8 | 12 | 6 | 8 | 12 | 0 | 0 | 8 |
| spot | 222 | 322 | 117 | 46 | 0 | 15 | 0 | 8 |
| fandisk | 165 | 243 | 75 | 38 | 21 | 8 | 0 | 8 |
| rocker-arm | 489 | 692 | 199 | 98 | 0 | 10 | 1 | 0 |
| cheburashka | 551 | 824 | 282 | 114 | 9 | 23 | 0 | 8 |
| stanford-bunny | 628 | 771 | 189 | 133 | 3 | 33 | 0 | 8 |

Total index is `4 * Euler characteristic`, so 8 for a genus-0 surface and 0 for
rocker-arm's genus-1 handle — a cheap end-to-end sanity check that the layout
agrees with the field it came from.

The rollout endpoint is a public `zremesher` quad method alongside
`quad-cover` / `field-aligned` / `instant-meshes`, with a CLI flag, a C ABI
surface and a Python binding. Until the representation settles, the layout stays
internal to the quadrangulate module — exposing it as stable API before the
later phases have exercised it would freeze the wrong shape.
