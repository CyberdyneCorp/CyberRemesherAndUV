# Proposal: fix-hole-fill-double-close

## Why

**The validity guarantee quotes a number that was a bug.** `remeshing-pipeline`
records topological validity as "0 defects on spot, fandisk, rocker-arm and
cheburashka, and 8 vs QuadriFlow's 38 on stanford-bunny". That 8 was read for
months as an inherent property of bunny's scan geometry. It was a defect in the
extractor's hole filler.

`IsolineExtractor::fixHoles` closes each boundary loop by calling
`fixHoleWithQuads(loop, true)` and then `fixHoleWithQuads(loop, false)`, relying
on the first pass to consume what it filled — `hole` is an in/out parameter. The
terminal 4-gon branch returned with `hole` still populated, so a loop that reduced
to exactly four edges was closed by the first call and closed **again**,
identically, by the second. Two coincident quads are edge-count-manifold on their
own, so nothing downstream rejected them; the pure-quad subdivision then gave each
its own face point and turned the shared rim into a genuine non-manifold edge.

Measured on the shipped default over five models × five densities (2600/3000/3400/
3800/4200 requested quads): **stanford-bunny 40 → 0 non-manifold edges** (8/8/0/16/8
before), the other four models 0 → 0 with 20 of 25 cells bit-identical. The one
non-identical clean cell (rocker-arm @2600) keeps an identical face list with
vertices moving ≤1.1e-5 — a relaxation shift, not a topology change.

This also explains bunny's long-recorded non-monotone defect count (8 at 2660
quads, 0 at 3004, 16 at 3308), which had been documented as unexplained density
noise. It was never noise; it was whether the traced graph happened to leave a
4-edge loop.

**Separately, the benchmark was comparing unequal densities.** `target_quad_count`
is a request the extractors undershoot and QuadriFlow overshoots — historically by
11–16% at the same request. Since `feature_error` falls roughly as `count^-0.5`,
that density gap was worth a large fraction of the Phase 3 feature-following gaps
the spec quotes. Both arms now go through a bounded count-match search.

## What Changes

- **Add a normative hole-fill contract**: each boundary loop is closed at most
  once, and extraction never emits two faces on the same vertex set. This is the
  property the bug violated, and it is now guarded by a regression test.
- **Restate the validity guarantee**: 0 defects on all five corpus models across
  requested densities 2600–4200, with the density range stated — a defect count
  without a density is not a claim.
- **Restate the feature-following gap at matched count**, and stop recording
  un-pinned integer-grid *phase* as its cause. Per-feature-edge integer
  constraints were implemented against that premise, verified exact on the maps
  they reach, and measured inert; the cause is open rather than known.

## Impact

- Behaviour: the default `quad-cover` path emits strictly fewer defects. No
  parameter, API or output-format change. Four of five corpus models are
  topologically unchanged.
- Specs: `remeshing-pipeline` — one requirement modified (two scenarios changed,
  one added).
- Tests: `tests/quadrangulate/test_seamless_solver.cpp` gains a torus-knot fixture
  and the "closed once, never twice" regression case;
  `examples/test_count_match.py` is registered with ctest.
