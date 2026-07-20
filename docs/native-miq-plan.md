# Native seamless-UV (QuadCover/MIQ) solver — scope & plan

> Scope + go/no-go for replacing the vendored-Geogram `quad_cover` dependency with a
> native seamless integer-grid parameterizer, so the `quad-cover` method stands alone
> without the ~102-TU AutoRemesher/Geogram vendored library. Written 2026-07-20 after
> M0–M4c landed (quad-cover at QuadriFlow parity via the vendored solver).

---

## Why / why not (read this first)

**Quality is already at parity.** quad-cover (via the in-process vendored Geogram
`quad_cover`) reaches irregular 1–4% — beating QuadriFlow on spot + rocker-arm, tying
bunny — with near-parity CV and best-or-tied median angle. A native solver does **not**
improve quality; the goal is purely to **drop the vendored dependency**:

| Property | Today (vendored, `-DCYBER_WITH_QUADCOVER=ON`) | Native |
|---|---|---|
| Quality | QuadriFlow parity | at best the same |
| Dependency | ~102 Geogram/AutoRemesher TUs, TBB, git-clone-on-configure | none (our tree only) |
| Build weight | heavy (opt-in, default OFF) | light |
| Licence | MIT/BSD (fine) | MIT (ours) |
| Effort remaining | **0 (done)** | **multi-week research** |

**Honest recommendation: LOW priority.** The vendored path is opt-in, licence-clean,
and already ships parity. The only wins are build-weight and self-containment. Do this
only when (a) we want quad-cover ON by default without forcing a Geogram build on every
consumer, or (b) a platform can't build the vendored lib. Otherwise the effort is better
spent elsewhere. This doc captures the design so it can be picked up when justified.

---

## What we already have (the native path is NOT from scratch)

The load-bearing pieces exist in our own tree — a native solve reuses them:

- **4-RoSy frame field + singularities** — `computePositionField(mesh, spacing, iters)`
  (`position_field.hpp`) already produces a smooth, feature-aligned per-vertex
  orientation field `q` (+ normal, + a continuous position field `o`). Field index /
  cone points are already handled by the integer extractor. **This is the frame field a
  QuadCover solve aligns to — reuse it, do not rebuild it.**
- **Sparse matrix-vector product** — `accel::SparseMatrix` + `spmv` (`primitives.hpp`,
  GPU-capable). A **Conjugate-Gradient** solver for the symmetric-positive-definite
  Poisson system the parameterization reduces to is a thin layer on top of `spmv` — no
  external linear-algebra dependency needed.
- **Min-cost flow** — `MinCostFlow` (`quad_extract.cpp`) already drives the integer
  extractor's global integer optimization; the same engine can round seam translations /
  balance integer jumps.
- **Eigen** — vendored at `examples/reference/autoremesher-src/thirdparty/eigen`
  (header-only) is available as a fallback sparse solver **without** linking Geogram, if
  CG convergence proves fragile on cone-heavy meshes.
- **Seamless validation** — `seamlessUvResidual(uv)` already measures integer-jump
  residual across interior edges; it is the acceptance gate for every milestone (0 ==
  seamless), exactly as it validated the harness UV.

## Algorithm choice: QuadCover over MIQ

**QuadCover** (Kälberer–Nieser–Polthier 2007) is the more tractable native target than
full MIQ (Bommes 2009):

- It parameterizes a **branched covering** induced by the frame field: away from
  singularities it is a plain harmonic/Poisson solve for `(u,v)`; singularities become
  branch points with prescribed cone angles. This is a **linear** system (CG-solvable)
  once the cut graph and integer jumps are fixed — no general mixed-integer program.
- MIQ's greedy integer rounding of the full system is more accurate but needs a robust
  sparse direct solver and a rounding loop; QuadCover reaches seamless integer grids with
  a linear solve + a smaller integer step on the seams. Start with QuadCover; consider
  MIQ-style rounding only if the residual gate isn't met.

## Milestones

**M0 — Scaffold + seam.** `computeSeamlessUvNative(mesh, edgeLen, adaptivity) ->
SeamlessUv`, guarded stub returning `valid=false`. Wire it as the FIRST choice in
`computeSeamlessUv` (native → in-process Geogram → subprocess harness), gated behind a
`CYBER_QC_NATIVE` opt-in until it validates, so nothing regresses. *Test:* stub returns
invalid, callers degrade.

**M1 — Frame field adapter + cut graph.** Adapt `computePositionField`'s `q` into the
per-triangle 4-RoSy representation QuadCover needs; detect singularities (field index ≠
0); build a **cut graph** (spanning tree connecting singularities + one loop per handle)
that opens the surface into a disk. *Test:* cut graph makes the mesh simply-connected
(Euler characteristic of the cut mesh = 1); singularity count matches the integer
extractor's.

**M2 — Seamless parameterization solve (THE research core).** Assemble the Poisson system
(gradient of `(u,v)` ≈ frame field × spacing) with the frame-field rotation transitions
across cut edges as constraints; solve with CG-on-`spmv`; round seam translations to
integers (min-cost flow) and re-solve until the integer-jump residual across interior
edges is ~0. *Gate:* `seamlessUvResidual < 1e-3` on the sphere (matching the harness's
0.000000 across 6273 edges). **This is the multi-week step.**

**M3 — Isotropic pre-remesh.** QuadCover wants a reasonably uniform triangle mesh (the
harness isotropically remeshes first). Reuse our own `isotropicRemesh` at the target edge
length before the solve (we already have it). *Test:* extracted quad count tracks the
target; feed M2's UV into the existing `extractIsolineQuads` → clean quads.

**M4 — Wire as default + drop the vendored lib.** Once M2/M3 pass the residual + parity
gates on the corpus, make `computeSeamlessUvNative` the default path, flip
`CYBER_WITH_QUADCOVER` semantics (native is built-in, vendored becomes an optional
cross-check), and quad-cover ships as the always-available default with no Geogram. *Gate:*
`11_benchmark.py` irregular/CV/median within noise of the vendored-solver numbers across
all 5 models; full suite green with no vendored dependency.

## Risks

- **CG convergence on cone-heavy / thin-feature meshes** — mitigations: Eigen sparse
  direct solver fallback (vendored, no Geogram), or a multigrid preconditioner on `spmv`.
- **Cut-graph quality** drives seam placement and thus singularity placement; a poor cut
  graph yields more irregulars than the vendored solver. This is where most of the tuning
  effort will go.
- **Parity is not guaranteed** — a from-scratch QuadCover may land at AutoRemesher's
  pre-tuning numbers (6–15%) before the frame-field/cut tuning that got us to 1–4%. Budget
  for the tuning, not just the solve.
