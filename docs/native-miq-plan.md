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

**M2d — ARAP distortion polish (closed surfaces) — ✅ DONE.** The plain field-aligned target
gradient has non-zero curl, so the Dirichlet (Poisson) projection introduces shear / non-uniform
scale; the integer isolines then cross the field obliquely and leave non-quad **caps** at cones,
each of which the pure-quad subdivision turns into a valence-3/5 irregular. `solveParameterization`
now runs a bounded ARAP **local-global** relaxation on the relaxed per-component Poisson *before*
the integer phase (`seamless_solver.cpp`):
- **Local.** Per face, take the current UV Jacobian `J` in the combed field frame `(e0,e1)` and
  find the closest rotation `R(theta)`, `theta = atan2(J_yx − J_xy, J_xx + J_yy)`, clamped to a
  wide ±60° cone so a sliver's noisy gradient can't fold the map.
- **Global.** Re-assemble only the divergence RHS with the target frame rotated by `theta`
  (the cotan Laplacian and the operator `A` are unchanged, so each re-solve is a warm-started CG),
  a few rounds. The refined RHS then feeds the integer-seamless reduction unchanged, which
  re-enforces exact seamlessness (`seamlessUvResidual` stays ~1e-6). `theta==0` reproduces the old
  field-aligned solve, so this only *relaxes* toward a locally rigid grid — it cannot move
  singularities (fixed by the cut/seam topology) and keeps the target norm at `1/spacing`, so the
  map stays bounded.
- **Gated to closed surfaces.** A free boundary has no downstream extractor cleanup safety net, so
  a boundary-hugging polish can flip the extracted mesh non-manifold there; a boundaried surface
  also measured *worse* under the polish. The solve skips ARAP when the mesh's boundary-edge
  fraction exceeds ~1 % (guarded by `seamless ARAP gate: open bowl ...`).

**Result (native, target 3000, pure-quads, quad-cover):** corpus irregular % drops
spot 5.4→4.3, fandisk 4.0→3.6, rocker-arm **8.0→5.5**, cheburashka 5.7→5.5, bunny 5.8 (unchanged,
gated); mean 5.8→4.9, worst 8.0→5.8, all watertight pure-quad, suite green.

**M3 — Isotropic pre-remesh + wiring — ✅ DONE.** `computeSeamlessUvNative` (was a stub) now
runs the full native pipeline: `triangulate` → `tagFeatureEdges` → **feature gate** (decline
sharp-feature meshes up front — the M2 solve does not constrain feature edges and is
ill-conditioned/divergent on them, so degrade to the vendored/subprocess/field-aligned path
instead of paying a slow solve) → `isotropicRemesh` at `targetEdgeLength` (reuses core) →
`buildSeamlessSetup` → `solveParameterization` → assemble a `SeamlessUv` from the per-corner UV
→ **divergence guard** (reject a UV whose cell-bbox is ≫ triangle count, so the isoline tracer
never enumerates an astronomical grid / hangs). Verified end to end with `CYBER_QC_NATIVE=1`,
Geogram OFF, no `CYBER_QUADCOVER_CLI`: a UV sphere yields **442 quads through the full pipeline**,
entirely native.

**M4 — Native is the stock-build default fallback — ✅ DONE.** `computeSeamlessUv` now tries the
**vendored** Geogram `quad_cover` FIRST (in-process when built with `CYBER_WITH_QUADCOVER`, else
the `CYBER_QUADCOVER_CLI` harness) — it keeps QuadriFlow parity (1–4%) where present — and falls
through to the **native** solver otherwise. So a stock build with no Geogram and no harness gets
the native solver by default (was: field-aligned per-island fallback at 9–16%). The
`CYBER_QC_NATIVE` opt-in gate is gone; `CYBER_QC_NO_NATIVE` force-disables native (fall through to
field-aligned) where speed/determinism matters. This flip was gated on M5+M6+M7 landing all the
readiness items — now met: native runs on the whole corpus, watertight, bounded, cancellable, at
**3.3–5.1% irregular** (below field-aligned's 9–16%; a hair above the vendored 2–4%). Verified: the
default (no env) remesh of spot yields 4.1% via native; `CYBER_QC_NO_NATIVE=1` yields 8.6% via
field-aligned in 0.5s; full ctest (unit + golden + document-invariants + api) green in ~62s. The
one remaining tradeoff is latency (a few seconds vs sub-second field-aligned), documented and
escapable; builds with the vendored solver are unaffected.

**M5 — Hardening: the native solve now RUNS on the whole corpus, does not diverge, and is
cancellable.** Four gaps closed:

1. **Feature meshes RUN natively (no more decline gate).** The old "decline if any feature edge"
   short-circuit is gone. Feature edges are re-tagged on the *remeshed* mesh (the isotropic stage
   preserves the crease geometry but drops the edge flags) and marked as **hard seams** in
   `buildSeamlessSetup`: the grid breaks along a crease so its isolines run along it, and the
   corners split there instead of forcing a continuous UV over a ~45° field mismatch.
2. **Integer-magnitude control replaces reject-only divergence.** The reduced integer solve now
   pins **one gauge per connected component** of the cut-open mesh (feature seams fragment the
   surface into patches; a single global gauge left the others on the Laplacian's constant
   nullspace → the ~1e9-cell blow-up). The relaxed phase likewise pins one vertex per component,
   and the rounded integer translations are clamped to an O(grid-extent) cap. `fandisk` etc. now
   produce a sane bounded UV instead of being rejected.
3. **Convergence + cancellation.** `solveParameterization`, the two Poisson CGs, the reduced
   masked-CG and the greedy rounding loop all thread and poll a `CancelToken` (≤64-iter latency);
   `computeSeamlessUv[Native]` check it between stages. A cancelled job returns an invalid result
   promptly and the caller degrades cleanly.
4. **Quad-count calibration.** The native grid honours the quadrangulator's `scaling` sweep as a
   solve-spacing multiplier, so the extracted count tracks the request instead of landing ~5× low
   (the dominant driver of the earlier high irregular %).

Corpus (`CYBER_QC_NATIVE=1`, no Geogram/harness, `target_quad_count=3000`, pure-quads): native
runs on **all 5** models — spot 5.4 %, fandisk 4.0 %, rocker-arm 8.0 %, cheburashka 5.7 %,
bunny 5.8 % irregular (was: spot 9.8 % native + four models falling back to field-aligned at
10–16 %), 10–21 s each, no divergence. Field-level cone reduction (extra smoothing sweeps) was
tried but does **not** move output irregular % here — the bottleneck is seam/extraction density,
not field cone count. Regression tests: sharp-cube runs bounded + seamless, and a pre-cancelled
token aborts the solve.

**M8 — Cone placement: multiresolution cross field + tube-aware coarsening — 🔬 EXPERIMENTAL.**
Motivated by the Stanford-bunny **ears**: on thin high-curvature tubes the single-level field
plants ~5× QuadriFlow's singularities (38 vs 8 irregular verts in the ear region), which shows up
globally as edge CV 0.33 vs 0.17 and normal err 12.8° vs 10.7°. A diagnose→adversarially-verify→
synthesize workflow (`.claude/workflows/quad-quality-push.js`) ranked the fixes; measurement then
**refuted the two cheap ones** — raising the feature-edge threshold (noisy, regresses CAD) and
curvature-weighted seam routing (net-negative corpus-wide, reverted). Two levers landed:

1. **Multiresolution cross field** (`computeCrossFieldFromOrientation`, gated by
   `CYBER_QC_CROSSFIELD_MULTIRES`). Derives the per-face cross from `computePositionField`'s
   coarse-to-fine per-vertex 4-RoSy orientation instead of single-level face smoothing — the
   global singularity placement a single level gets stuck on. Robustly lowers irregular % on
   smooth models (spot 4.7→3.9, fandisk 3.2→2.5 + CV 0.27→0.20, cheburashka 5.5→4.0, rocker
   4.6→4.0). **Off by default**: the stock quad-cover path and the field-aligned engine are
   unchanged.
2. **Tube-aware coarsening** (`coarsen` in `position_field.cpp`, `CYBER_QC_COARSEN_MINDOT`,
   default 0.5, **on**). Matches each node to its most normal-coherent neighbour and refuses to
   pair across a fold, so the hierarchy stops bridging thin necks (front/back of the ear) into a
   coarse node with a meaningless averaged normal. Without it, multires *doubled* the bunny ear
   count (38→73); with it, 73→46. Neutral-to-positive for the position-field/integer engines it
   also feeds (bunny 20→19, 13→12); off the stock quad-cover path.

**Open — the bunny ear is not yet robustly fixed.** Multires+tube-aware reaches ear 30 / CV 0.30 /
Nerr 11.3 at `CYBER_QC_COARSEN_MINDOT=0.9` (all below stock) but with irr 8.6 — the quad-count
calibration's ±10 run-to-run swing swamps the signal. The next blocker is **extraction
determinism** (fix the scaling so the landed count, and thus the singularity count, is stable),
after which mindot can be tuned and multires considered for default. Regression test:
`test_crossfield.cpp` "orientation-derived cross field is a unit 4-RoSy aligned to a flat grid".

## Risks

- **CG convergence on cone-heavy / thin-feature meshes** — mitigations: Eigen sparse
  direct solver fallback (vendored, no Geogram), or a multigrid preconditioner on `spmv`.
- **Cut-graph quality** drives seam placement and thus singularity placement; a poor cut
  graph yields more irregulars than the vendored solver. This is where most of the tuning
  effort will go.
- **Parity is not guaranteed** — a from-scratch QuadCover may land at AutoRemesher's
  pre-tuning numbers (6–15%) before the frame-field/cut tuning that got us to 1–4%. Budget
  for the tuning, not just the solve.
