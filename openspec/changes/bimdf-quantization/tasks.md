# Tasks: bimdf-quantization

- [x] 1. Motorcycle-graph / T-mesh extraction from the seamless parameterization
       (trace separatrices from cones and feature junctions; arcs = T-mesh edges
       with real-valued lengths from the relaxed solve)
       — landed (`src/quadrangulate/src/bimdf_quantize.cpp`): lockstep tracing,
       fold-robust contour following, exact symbolic arc lengths, crease
       chains, quad-patch extraction. Clean on CAD (box_sharp: 8 nodes /
       12 arcs / 6 patches, exprErr 0). The "work-mesh crack" fallbacks were
       non-manifold edges made by the isotropic collapse pass (no link
       condition) — fixed in the mesh kernel (`Mesh::collapseEdge` +
       CollapsePass obstruction flips); box_sharp@16000 is now injectable.
       Fold tolerance at cone launches landed (signed UV wedge ranking +
       rescue candidates, entry-edge re-crossing, regular-vertex
       pass-through, graceful ray abandonment): every corpus mesh now traces
       a complete T-mesh (spot 18/194 non-quad patches, was 107/222;
       nefertiti@4000 596/2316; armadillo@4000 294/1386); the residual
       non-quad orbits are fold-corrupted corner SECTORS (rotation side),
       dominated by degenerate ≤2-corner orbits.
- [x] 2. Bi-directed flow-network construction per the Bi-MDF paper: arc
       variables = integer isoline counts, node conservation from T-mesh faces,
       feature-pinned arcs fixed (gap-#1 semantics preserved)
       — split-node template in half-cell units; crease arcs are ordinary
       arcs (their transverse lattice pins live in the c_e integers, which the
       back-substitution leaves to the reduction); min-one bounds are the
       degenerate-assignment guard with `raisedToMin` reporting.
- [x] 3. In-tree min-deviation-flow solver (successive shortest augmenting path
       with convex costs; no third-party deps)
       — S1 approximate path: T-join parity + Hochbaum double cover on the
       extracted SPFA `MinCostFlow` (`min_cost_flow.hpp`), convex PWL costs as
       parallel arcs; unit-tested (`tests/quadrangulate/test_bimdf_quantize.cpp`).
       S2 (M=2 matching refinement) not started.
- [x] 4. Wire behind CYBER_QC_BIMDF into solveSeamlessReduced as the integer
       assignment, greedy rounding fallback + kill switch; byte-exact revert
       — back-substitution through the run's actual pivot expressions, one
       batch pin + single direct.resolve, greedy finishes the remainder;
       `CYBER_QC_BIMDF=report` = A/B mode without injection; flag off is
       byte-exact (ctest 14/14; box output geometry identical on/off).
- [x] 5. Corpus + multi-density A/B vs greedy (bench gates + cad_sweep.py);
       flip default only on a clean win; ROADMAP entry with numbers
       — STILL BLOCKED, three stages further along (branch
       `feat/bimdf-tail`, after `feat/bimdf-sectors`): (i) QEx Alg-8
       sectors + even-sum polygonal template (see ROADMAP 2026-08-02);
       (ii) twin-arc merge / bigon collapse with phantom quarters (rejected
       orbits nefertiti@4000 377 → 143, deg ≤2 224 → 49; armadillo 174 →
       45; spot 11 → 3); (iii) LOCAL CONTAINMENT — failed rays, rejected
       orbits and relax-inconsistent quads no longer refuse the T-mesh
       (their arcs are excluded from injection, greedy rounds them
       locally), plus solve-side support (fixed boundary constants,
       T-join ground, infeasibility drop valve, side-level anti-collapse
       floors, ±1 repair sweep) — every corpus organic AND the open-boundary
       stanford-bunny now BUILD and SOLVE (nefertiti@4000: 1896/2078
       patches, halfIntegral 0, sideViolation 2; bunny: 441/471). The
       injection blocker is now precisely the JOINT HALF-INTEGER LATTICE:
       ~half the eliminated pivots land at frac exactly 0.5 (spot 71/136
       clean, nefertiti 861/1649) because the network does not model the
       parity coupling of half-integer cone positions; partial pinning
       (CYBER_QC_BIMDF=force) measured: bunny ears 37 → 31 with per-arc
       floor 0, but nefertiti sing 396 → 431 — so DEFAULT injection
       requires full consistency + zero contained arcs (A/B: every cell
       hash SAME, dE identical; bench check green both states; box_sharp
       8 cones / 1.00 / 0.0°; ctest 14/14). Gates spot@3000 pure flow
       762.9 (≥ 1000) and nefertiti@4000 pure sing 419 (≤ 200) NOT met.
       Ranked next: parity-aware quantization (lattice constraints into the
       Bi-MDF), guided rounding (integral-by-construction greedy toward the
       flow targets), boundary arcs. Default NOT flipped.
       — ROUND 4 (branch `feat/bimdf-guided`): GUIDED ROUNDING landed as
       `CYBER_QC_BIMDF=guided` — the greedy schedule unchanged, but its
       re-solves carry a quadratic ATTRACTION of the T-mesh arc rows
       toward a floors-OFF Bi-MDF solve (the shipped side-floor solve has
       no headroom: energy 2711 vs greedy-realized 1879 on
       nefertiti@4000; floors-off 1289 — the round-3 "1309" headroom),
       final solve unattracted, mu 1.0 (`CYBER_QC_BIMDF_MU`, bunny basin
       0.75-1.25 flat). Coordinate-wise floor/ceil steering was measured
       first and always lost (nefertiti sing 396 → 418/399/422 over three
       scoring variants) — recorded as a falsified design. Health gates
       auto-refuse to pure greedy: sideMismatch <= 0.2 (nefertiti 0.435 /
       armadillo 0.496 regress when steered) and crease arcs <= 1%
       (cylinder@4500 pure collapsed quads 3792 → 1702 / haus 0.05 when
       steered; CAD keeps exact injection, byte-identical to flag-on at
       box_sharp@1000 pure, injected=6). RESULTS where engaged: bunny@3000
       sing 135 → 94, EAR IRREGULARS 37 → 19 — beats QuadriFlow's 20;
       spot@3000 pure flow_loop 762.9 → 1274 (gate ≥ 1000 met on the
       mean; loops 7 → 4, quads −4.6% — count-match arguable), sing 66 →
       65. nefertiti pure sing gate UNREACHABLE by any rounding lever: the
       pure-arm T-mesh never builds ("trail density blowup") — tracer
       wall, promoted to the top of the ranked levers. PARITY-AWARE (a)
       assessed and dropped with evidence: the classes are a coupled
       GF(2) system beyond the graphic T-join, and exact flow realization
       is harmful exactly where injection is blocked (damaged organics
       regress under force AND attraction) while healthy meshes already
       bank the win via attraction. A/B: off vs report hash-SAME 22/22;
       guided differs only where engaged; ctest 14/14; bench check green
       both flag states; flag-off byte-exact vs the main-branch binary
       (4 cells, mtllib-normalized). Default NOT flipped (opt-in lever;
       organic-scoped wins; spot mixed recall −0.023 is a real cost).
       — ROUND 5 (branch `feat/bimdf-boundary`): the PURE-ARM TRACER WALL
       is fixed — the "trail density blowup" was ONE self-spiralling
       separatrix per mesh (511/512 segments in the tripping face were
       its own trail; the crossing scan exempted own trails from
       T-junction termination). Landed: self-hit termination (canonical
       motorcycle-graph rule, 0.25-cell curve-age margin), guard-trip
       abandonment + containment (whole-T-mesh refusal gone), and
       fan-anchor failures degrading into containment. Every pure
       organic now BUILDS and SOLVES (nefertiti@4000 pure 2644 patches /
       371 contained, armadillo 1392/164, bunny 191/322). GATE MEASURED
       via the CYBER_QC_BIMDF_HEALTH override: nefertiti pure sing 419
       (greedy) → 411 guided at mu=1 (mu=2: 310 but haus 0.0072 →
       0.0194, quads −14%) — **≤ 200 NOT met**; engaging fold-damaged
       meshes regressed every measured cell (nef mixed 396 → 400, arm
       mixed 255 → 281, arm pure 273 → 293), so the whole-mesh health
       gate stays and the wall is now the quarter-density fold damage
       (1297 cones / 110 degraded nodes), not the tracer or quantizer.
       QGP BOUNDARY ARCS landed opt-in (`CYBER_QC_BIMDF_BARC=1`):
       boundary loops → arc chains with ray-hit T-nodes, min-one floors,
       noExpr (free boundary not grid-aligned), fixed constants in the
       flow (a variable/hub design measured bit-identical while adding
       the copy-crossing structure round 3 found half-integral; the
       floors solve's half=12 on bunny comes from the boundary patches
       under either encoding, the floors-off steer solve stays half=0).
       Coverage bunny mixed 441/471 → 457/485
       (failedRays 12 → 3) but DEFAULT OFF: the recovered regions
       reshape the flow globally and the flagship regresses across the
       mu basin (ears 19 → 31-37, sing 94 → 115-129) — round-3
       containment is load-bearing. Gates: ctest 14/14; bench check
       green off/report/guided; off-vs-report hash-SAME 22/22; flag-off
       byte-exact vs main binary (4 cells); CAD spot-check 31/32
       unchanged (the 1 = round-3-documented box_sharp@400 pure exact
       injection); bunny ears 19 / sing 94 exactly reproduced;
       deterministic. Default NOT flipped.
       — ROUND 6 (branch `feat/fold-repair`): SUBSTRATE FOLD REPAIR
       landed. Fold census re-landed gated (`CYBER_FOLD_DIAG`,
       per-phase counts with cone-distance attribution; nefertiti@4000
       pure reduced-preint 482 folds, 89% cone-adjacent — the diagnosis
       reproduced at quarter density). QGP §7.1 dynamic
       re-linearization BUILT and MEASURED NEGATIVE (three
       formulations; best stable variant only 482 → 382 and its
       margin-slivers break the tracer) — env-gated OFF
       (`CYBER_QC_FOLD_RELIN`); rewinding >2π wedges is nonlinear.
       The WORKING lever is Garanzha-2021 regularized-Winslow
       untangling over the reduced free basis (exactly seamless by
       construction, double precision, L-BFGS, substrate-only, `auto`
       above 1% folded faces): folds 482 → 45 / 198 → 9 / 41 → 0 in
       0.15-0.5s; `bimdf::Charts` went double (float substrates split
       twin separatrix levels by one ulp → collinear-overlap refusal).
       Chain re-measure: contained 371 → 314, sideMismatch 0.731 →
       0.627, but degraded 110 → 125 — classification does NOT clear.
       GATE STILL NOT MET: best sing 333 (geometry-damaged) / 358
       (sane, multires+guided+untangle) vs ≤ 200 target, QF 80. The
       measured residue is CONE COUNT: 1297 field cones, multires
       removes only 29 (1297 → 1268); quantization already collapses
       ~⅔; no map-level lever moves the budget → redirect to
       field-level dipole annihilation. Gates: ctest 14/14, bench
       check green ×3, bunny ears 19 / sing 94 exact, box exact
       injection, CAD 16/16 + organics 5/5 + off-vs-report 18/18
       metric-SAME vs main; byte-hash gates retired (main itself is
       run-to-run non-byte-deterministic, metrics identical). Default
       NOT flipped; guided stays opt-in and health-gated.
       — ROUND 7 (branch `feat/wrinkle-web`): the CONE BUDGET moved for
       the first time — resolution-aware feature DEMOTION at the field
       level (quadcover_extractor/crossfield/seamless_solver, default
       ON, kill switch `CYBER_QC_NO_FEATURE_DEMOTE`). Measured first
       (`CYBER_QC_FIELD_STATS` web census + dipole-blocking census):
       nefertiti@4000 pure carries ZERO resolvable feature chains, yet
       782 tagged knife edges + a 45°-crease align web freezing 43% of
       the field pin 89% of its 1297 cones and web-block 387 of 464
       unmatched +cones. Demotion (coarse-substrate gate ≥4 input
       edges per cell; resolvable = traces the original's ≥2-cell
       crease network OR bends ≥45° persistently at ±{0.3,0.6,1}h on
       the ORIGINAL surface with 20° side coherence) opens the field:
       cones 1297 → 633 (armadillo 651 → 266), web-blocked 387 → 69.
       Output: nefertiti pure greedy 419 → 220, guided 248, multires
       176 / multires+guided 150 — WITH better geometry (haus 0.0072 →
       0.0051, recall 0.738 → 0.780); armadillo greedy 159 / guided
       143 (gate ≤200 MET on the default path). Nefertiti ≤200 still
       needs env-gated multires; its torus handle regression is now
       the binding blocker. Bunny/CAD/spot disengage via the
       coarseness gate (bunny ears 19 / sing 94 exact; CAD 16/16
       metric-SAME, demotion never fires). ctest 14/14, bench check
       green ×3, flag-off reproduces round-6 baselines exactly.
       — ROUND 8 (branch `feat/multires-default`): the GATE CLOSES on
       the stock path. The multires torus regression was diagnosed as a
       TRAPPED HOLONOMY WINDING (per-level dumps: twist coherence
       R 0.128 vs stock 0.519, twist swinging 5°→84° around the major
       generator; depth-cap sweep proved coarse aliasing was NOT the
       cause — even a no-coarsening hierarchy regressed) and fixed by
       coherent BFS-transport seeding of unanchored,
       non-simply-connected coarse components (per-component Euler
       characteristic; anchored components measured worse under a
       transported gauge — nefertiti 204 vs 176 — and simply-connected
       ones too — sphere haus 0.0070 → 0.0218 — so both keep the
       historical init bit-for-bit). Torus flips to a WIN (sing
       62 → 59, haus 0.0250 → 0.0158) and
       `CYBER_QC_CROSSFIELD_MULTIRES` became the DEFAULT (kill switch
       `CYBER_QC_NO_CROSSFIELD_MULTIRES`, verified to reproduce the
       round-7 stock numbers exactly). **DEFAULT-PATH RESULTS:
       nefertiti@4000 pure greedy 176 (haus 0.0052 ≤ 0.010), guided
       (health override) 150 — gate ≤ 200 MET with no env vars;
       armadillo 119 greedy / 112 guided; spot pure 55; bunny default
       guided ears 16 / sing 82; sphere 21 (was 37); cylinder 5 (was
       9); box_sharp 8/1.00/0.0° bit-identical.** Baselines
       deliberately re-recorded (sphere/torus/cylinder improvements).
       Gates: ctest 14/14; bench check OK ×4 (default/report/guided/
       off); CAD box+cylinder × 4 densities × both arms — box 8/8
       bit-identical on-vs-off, cylinder improved (4500: sing 29 → 18,
       angle 30.5 → 5.2), arms metric-SAME per config (box_sharp@400
       arm split is pre-existing, reproduced bit-for-bit by stock).
       Remaining (tracked in ROADMAP, outside this change's gate):
       guided health on demoted substrates (sideMismatch 0.534 still
       refuses unaided), QuadriFlow's residual ~2× (cone placement).
