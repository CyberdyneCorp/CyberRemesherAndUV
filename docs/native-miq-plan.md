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

**M1 — Frame field adapter + cut graph — ✅ DONE (genus 0).** `buildSeamlessSetup` in
`seamless_solver.{hpp,cpp}`: reuses the per-face `CrossField` (`computeCrossField`), and
adds per-edge period jumps, per-vertex cross-field singularity indices, and a cut graph.
The index is computed by walking each interior one-ring in a consistent CCW sense
(following face orientation) and rounding (Σ minimal spoke residuals + angle defect) /
(π/2); this is validated rigorously by **Poincaré–Hopf: Σ index == 4·χ** — measured 8 on
a sphere and on spot (χ=2), 0 on a torus (χ=0). The cut graph is a spanning tree over the
singular vertices routed along mesh edges (BFS); slitting a closed genus-0 surface along
it opens it to a **disk (cut-open χ == 1)**, validated by `cutOpenEulerCharacteristic`
(union-find over face-corners). *Tests:* `test_seamless_solver.cpp` (sphere Σ=8 + disk,
torus Σ=0, empty→invalid). **TODO for genus>0:** add homology-generator (handle) loops via
tree-cotree so a torus also opens to a disk — genus 0 (characters, most models) works now.

**M2a — Relaxed Poisson solve — ✅ DONE.** `solveParameterization` solves the seamless UV
on the mesh **cut open along the seam** (a closed surface can't carry a globally continuous
integer-grid UV, so seam vertices are duplicated via union-find over face-corners — the same
corners the cut Euler check merges). Per coordinate it assembles the **cotangent Laplacian**
+ divergence RHS from the **combed** frame field (`combField`: BFS over non-cut edges so the
cross is continuous on the disk) and solves `L u = b_u`, `L v = b_v` with **Conjugate
Gradient on `accel::spmv`** (`conjugateGradient`), pinning one vertex for SPD. Output is
per-corner UV. **Working end to end on real closed surfaces with no Geogram / no harness:**
sphere → 232 quads, spot → 742 quads via `extractIsolineQuads`. As expected for the RELAXED
(pre-rounding) solve the seam translations are non-integer, so `seamlessUvResidual ≈ 0.5`.
Tested in `test_seamless_solver.cpp` (cut duplicates seam verts, CG converges, UV varies).

**M2b/c — Integer seam rounding (THE remaining core, DIAGNOSED — not yet done).** The seam
must become a *rigid* grid symmetry `uv_B = R^r·uv_A + t` with `t` an integer 2-vector shared
by both endpoints of each cut edge. A relaxed-solve-then-penalize approach was built and
measured, and it **does not converge to a rigid seam** — precise diagnosis:
- Rounding `t` per endpoint independently makes each endpoint integer (my-convention residual
  0) but `seamlessUvResidual` stays ~0.5, because the two endpoints of a cut edge then carry
  *different* translations, so the edge is not a single rigid map.
- Forcing one shared `t` per cut edge (rounded from the two endpoints' average, with `r` taken
  from the relaxed UV geometry to match the residual metric's `bestK`) leaves my-convention
  residual ~0.5: the two endpoints genuinely *want* different integers, i.e. the relaxed seam
  is **not rigid** even geometrically. The base Dirichlet energy relaxes the two independent
  cut sides into a non-rigid state that a post-hoc penalty (even λ=1e4) cannot rigidify.

**Conclusion:** exact seamlessness needs the seam rotation coupled as a **hard constraint
during the solve** — the two cut sides are not independent DOF; side B is `R^r·`side A plus a
single integer translation propagated consistently along the seam (MIQ-style: greedily round
one integer transition, fix it, re-solve, repeat).

**M2c — Constrained integer-seamless solve — ✅ DONE (genus-0, low-singularity).**
`solveSeamlessConstrained` (in `seamless_solver.cpp`) re-solves the Dirichlet energy with the
seam transitions as HARD linear equality constraints and greedily rounds the integer
translations:
- **Rigidity as equality constraints.** For every seam edge the shared edge vector must
  transform rigidly by `R^rho` (`uv_B(b)-uv_B(a) = R^rho[uv_A(b)-uv_A(a)]`), with `rho`
  estimated from the relaxed UV so it matches the residual metric's `bestK`. This makes the
  seam transition a single well-defined `t` per edge (equal at both endpoints), constant
  along a constant-`rho` seam segment.
- **KKT via a range-space (dual Schur) solve.** The energy Hessian is `blkdiag(Lp,Lp)` with
  `Lp` the cotan Laplacian pinned at one singularity (SPD); the constrained system is solved
  in the range of `Lp^{-1}`, so each constraint costs only a well-conditioned CG solve
  (`accel::spmv`) — no indefinite KKT, no external solver. Columns are cached across rounding
  steps. A tiny ridge absorbs the redundant-but-consistent rigidity rows (cone holonomy).
- **Greedy integer rounding.** One integer translation per seam *segment*; round the
  most-confident (closest-to-integer), pin it, re-solve, repeat, so the continuous DOF absorb
  each rounding and closure stays consistent. Singularities are deliberately *not* pinned to
  the lattice — their cone fixed points are determined by the segment translations, and
  pinning them independently over-constrains the system (breaks rigidity, blows up the scale).

**Result:** on the UV sphere `seamlessUvResidual` drops from ~0.5 to **2.4e-7** (< 1e-3 gate)
and `extractIsolineQuads` yields a clean quad mesh (regression test `seamless M2c ...` in
`test_seamless_solver.cpp`).

**M2c-sparse — Scalable constraint-elimination MIQ — ✅ DONE (many singularities).**
`solveSeamlessReduced` replaces the dense dual with a sparse variable reduction, so it scales to
hundreds of cones and reconciles branch-point holonomy exactly (the dense path dropped one weld
per cut cycle and left residual ~0.5 on branching cuts):
- **Integer translations as variables.** Promoting the per-seam-edge integer translation `t` to
  extra variables `z = [u | v | t]` makes the seam rigidity `uv_B = R^rho uv_A + t` and the gauge
  pin HOMOGENEOUS linear constraints. `rho` is taken from the **combed field**
  (`rho = comb[B] - comb[A] - periodJump`, mod 4), the exact grid symmetry the Dirichlet energy
  aligns to — so the holonomy summed around any cone/junction is consistent.
- **Exact Gauss-Jordan reduction.** Every rotation/translation coefficient is ±1, so exact
  elimination reduces `z` to independent DOF `w` with a sparse map `z = T w`. Branch-point
  holonomy falls out automatically: a regular junction closes to a pure-integer row that
  eliminates one redundant translation; a cone closes to a pin of its representative. The
  surviving independent integers are therefore **unconstrained** — rounding them to *any* integers
  keeps the map seamless (dependent integers stay integer, cones land on the half-integer lattice).
- **Reduced CG + batched rounding.** The reduced Dirichlet energy `T^T blkdiag(L,L) T` is
  minimised by Conjugate Gradient applied implicitly through three `accel::spmv` (no dense reduced
  Hessian). Integers are rounded in batches (pin every confidently-near-integer value, re-solve,
  repeat) with the continuous solve warm-started across rounds, so a 350-seam mesh takes a few
  hundred CG sweeps, not one solve per integer.

**Result:** `spot` (92 cones, ~350 seam edges) reaches `seamlessUvResidual` **~1e-6** (< 1e-3
gate) with `extractIsolineQuads` yielding a ~600-cell quad-dominant mesh (~86 % valence-4). A
branching-cut regression (`seamless M2c: branching-cut ...`, a cube-sphere with 8 cones) guards
the holonomy reconciliation. No seam cap, no dense dual, no external solver.

**Still open:** improve field feature-alignment (a diagonally-triangulated flat grid pulls the
cross field ~27° off-axis).

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
