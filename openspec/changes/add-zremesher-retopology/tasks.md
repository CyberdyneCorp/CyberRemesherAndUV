# Tasks: add-zremesher-retopology

Implementation order follows the design's Phase A-G sequencing. Every phase
lands behind a flag with the shipped pipeline as the byte-exact default until
its own gate is measured green.

## Phase A — Structural refactor

- [x] A1. Promote the Bi-MDF T-mesh to a reusable `TopologyLayout`
       (`topology_layout.hpp/.cpp`): nodes (singularity / boundary corner /
       feature corner / T-junction / guide anchor / symmetry anchor), arcs
       (separatrix / feature / boundary / guide / symmetry) carrying samples,
       desired and quantized lengths, and patches with ordered boundary sides.
       `bimdf_quantize` keeps only quantization and consumes the layout.
       Gate: byte-identical output on every currently successful Bi-MDF case.
       — landed. The split is by RESPONSIBILITY rather than by moving 3000
       lines of tracing: `bimdf::TMesh` stays the QUANTIZATION view (arc
       lengths + exact symbolic lengths over the solver's promoted variables),
       and `TopologyLayout` is the GEOMETRIC/COMBINATORIAL view with no solver
       variables. Node and arc ids are shared. The tracer gained geometry
       capture (`Charts::captureGeometry` → `TMesh::nodeGeom`/`arcGeom`):
       T-node positions at creation, separatrix polylines sliced out of the
       walk trail by the events' own monotone curve parameter, crease/boundary
       polylines from the chain vertices. Nothing in `solveBimdf` reads any of
       it, so capture cannot move an assignment — verified byte-identical
       output with capture on vs off across all six corpus models.
- [x] A2. Deterministic layout validation (`validateTopologyLayout`) covering
       the Appendix-B invariants, plus layout debug export (JSON report and a
       polyline OBJ) and stats. Gate: every currently successful layout
       validates.
       — landed behind `CYBER_ZR_LAYOUT` (=1 reports; a value is a path prefix
       and also writes `<prefix>.json` / `<prefix>.obj`). Validation separates
       HARD violations (corrupt graph: bad ids, non-finite, out-of-range,
       dangling arcs) from LOCAL ones (a patch whose boundary walk does not
       close), mirroring the tracer's existing containment: the local case is
       reported per patch and its arcs are excluded, the sound remainder
       proceeds. All six corpus models validate; rocker-arm carries one
       contained non-closing patch of 199, which is a Phase B target.
       `examples/21_topology_layout.py` renders the layout over the quads.

## Phase B — Organic robustness

- [ ] B1. Make separatrix / contour tracing independent of UV triangle
       orientation (combinatorial quarter-frame transport, not signed UV area).
- [ ] B2. Replace fold-sensitive patch-sector classification with a
       quarter-frame / combinatorial classification.
- [ ] B3. Deterministic exact-vertex crossing rules (ties broken by a stable
       key, never by floating-point sign).
       Gate: zero organic T-mesh fallbacks caused by folded patch sectors on
       spot / fandisk / nefertiti; synthetic negative-index cone fixture passes.

## Phase C — Topology quality

- [ ] C1. `GeometryAnalysis` (normalized curvature, thin-feature risk,
       feature/boundary influence) computed once per solve and cached.
- [ ] C2. Layout-aware weighted singularity cost + metric reporting.
- [ ] C3. Deterministic local singularity relocation under a hard total-index
       invariant, rebuilding dependent cut/layout data.
- [ ] C4. Conservative dipole cancellation on the layout.
       Gate: weighted singularity cost improves, raw topological defects stay
       zero, geometry metrics do not regress materially.

## Phase D — Unified density

- [ ] D1. `SizingField` h(x) fusing painted density, curvature, feature
       proximity and local thickness into one per-face target spacing.
- [ ] D2. Feed the sizing field into Bi-MDF arc target lengths.
- [ ] D3. Feed it into the position field and the final projected optimization.
- [ ] D4. Legacy behavior byte-identical when the sizing field is disabled.
       Gate: painted-density, target-count and thin-feature regression suites.

## Phase E — Artist topology guides

- [ ] E1. Guide mode (orientation vs topology) in the core guidance model and
       its serialization.
- [ ] E2. Orientation mode stays byte-compatible with today's flow guides.
- [ ] E3. Deterministic projection of topology guides to surface paths.
- [ ] E4. Insert topology guides into `TopologyLayout` as first-class arcs.
- [ ] E5. Closed topology guides (loop guides).
       Gate: sphere-equator, eye-loop and mouth-loop fixtures show measurable
       topology-edge adherence.

## Phase F — Semantic boundaries and symmetry

- [ ] F1. `ConstraintField` (semantic / group / user-preserved boundaries).
- [ ] F2. Connect groups, material boundaries and user-preserved edges to the
       field pinning, the layout and the sizing field.
- [ ] F3. Forced X/Y/Z exact half-mesh solve producing mirrored connectivity,
       not merely mirrored positions.
- [ ] F4. Automatic symmetry detection, only once forced symmetry is solid.

## Phase G — Candidate selection

- [ ] G1. Build native and multiresolution cross-field candidates.
- [ ] G2. Common `layout_score` over geometry, quad shape, topology, flow,
       guide adherence, features and symmetry.
- [ ] G3. `Best` mode runs both candidates and picks by score with a stable
       tie-break.
- [ ] G4. `Balanced` mode predicts or cheaply probes instead of solving both.
       Gate: `Best` is never worse than either candidate by the score, plus a
       manual visual inspection so the score itself does not become the bug.

## Product surface

- [ ] P1. Public `zremesher` quad method: CLI `--quad-method zremesher` plus
       its flags, C API and Python binding, documented in `examples/README.md`
       and `docs/`.
- [ ] P2. Example scripts demonstrating the layout, topology guides, symmetry
       and quality modes on the bundled corpus.
- [ ] P3. Benchmark corpus additions (character and hard-surface) and the
       release gates from the design's acceptance table.
