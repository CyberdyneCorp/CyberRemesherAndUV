# Proposal: add-zremesher-retopology

## Why

The engine already owns most of the low-level machinery a ZRemesher-class
automatic retopologizer needs: 4-RoSy cross fields with transport-aware
smoothing, crease/boundary alignment, flow guides as soft directional
constraints, painted density, a native seamless parameterization, QuadCover
isoline extraction, a multiresolution position-field path, pure-quad
post-processing, motorcycle/T-mesh tracing, and a Bi-MDF global integer
quantizer over an in-tree min-cost flow.

What it does not own is **intentional global topology layout**. Today the
system answers:

> Given a directional field and an integer grid, generate valid field-aligned
> quads.

An artist-facing retopologizer must answer:

> What edge-loop and patch layout should this shape have, where should
> extraordinary vertices live, where do loops start, terminate and merge, and
> how do artist hints change that layout?

The topology is currently an *emergent* consequence of field + grid +
extraction. It lives implicitly inside `bimdf_quantize.cpp`, which owns
tracing, T-mesh construction, symbolic lengths, patch validation, parity,
quantization, injection and diagnostics all at once. Every improvement that
needs to reason about loops — singularity placement, topology guides, symmetry,
semantic groups, patch-grid extraction, quality scoring, debug rendering —
would otherwise add another conditional inside that one file.

## What Changes

- **A first-class `TopologyLayout`** (nodes / arcs / patches) is extracted out
  of the Bi-MDF T-mesh and becomes the reusable intermediate artifact every
  later stage consumes. Quantization behavior is unchanged and byte-exact.
- **Fold-robust layout tracing** so relaxed-UV foldovers degrade the *geometry*
  of a layout arc without corrupting the layout *graph* — the single largest
  unlock for the existing Bi-MDF solver on organic meshes.
- **Singularity placement optimization** against a topology-aware energy
  (curvature, feature and guide proximity, thin-feature risk, boundary),
  under a hard Euler/total-index invariant.
- **A unified `SizingField` h(x)** so painted density, curvature, feature
  proximity and local thickness feed one sizing decision instead of four
  independent knobs.
- **Local-feature-size preservation** so thin plates and tubes keep at least
  the quad rings needed to stay non-degenerate.
- **Topology-affecting flow guides**: guides gain an explicit
  orientation-vs-topology mode, so a closed guide can force a real continuous
  edge loop rather than only biasing the field.
- **Semantic boundary constraints** (`ConstraintField`), the PolyGroup-like
  input the layout, field pinning and sizing all consult.
- **Exact topology symmetry** — mirrored *connectivity*, not merely mirrored
  vertex positions.
- **Patch-grid extraction** driven by the quantized layout, and field-aware
  projected quad optimization that does not undo singularity decisions.
- **Quality-driven candidate selection** (fast / balanced / best) scoring
  complete candidate solves instead of trusting a single pipeline.
- A public **`zremesher` quad method** exposing the above through the CLI,
  the C API and the Python bindings, alongside the existing strategies.

## Impact

- Affected specs: `remeshing-pipeline` (topology-layout stage, zremesher
  strategy), `remeshing-parameters` (sizing, symmetry, guide mode, quality
  mode), `cli-headless` (`--quad-method zremesher` and its flags),
  `engine-bindings` (Python/C surface).
- Affected code: `src/quadrangulate` (new `topology_layout`,
  `singularity_optimizer`, `sizing_field`, `layout_score`, `symmetry_layout`;
  `bimdf_quantize` reduced to quantization), `src/core` (parameters,
  guidance, pipeline dispatch), `apps/cli`, `capi`, `blender`/Python bindings.
- No new third-party dependencies; the GPL discipline around quadwild /
  libSatsuma is unchanged (papers only, no vendored source).
- Rollout: every stage lands behind a flag with the existing pipeline as the
  byte-exact default until a full-corpus, multi-density win is measured.
- Exit gate: on the benchmark corpus, `zremesher` SHALL have zero topology
  defects and zero invalid layout patches, weighted singularity cost no worse
  than the shipped quad-cover default, feature recall no worse, and exact
  topological symmetry when symmetry is requested.
