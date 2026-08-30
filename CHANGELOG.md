# Changelog

> Note: releases 0.3.0, 0.4.0 and 0.5.0 were tagged without changelog entries;
> their content is recorded in `docs/ROADMAP.md`. Entries resume here.

## [Unreleased]

### Added

- **Topology layout — the first stage of the ZRemesher-class retopology work**
  (`openspec/changes/add-zremesher-retopology`, Phase A). The quad topology the
  engine produces has always been an *emergent* consequence of cross field +
  seamless grid + isoline extraction, which is mathematically sound but leaves
  nothing to reason about when the question is "where should the edge loops go".
  `cyber::remesh::TopologyLayout` makes that structure explicit: nodes
  (singularities, feature and boundary corners, T-junctions), arcs (the
  separatrix, crease and boundary curves between them, with their traced 3D
  polylines and lengths) and the patches those arcs bound.

  The split with the existing Bi-MDF tracer is by responsibility, not by moving
  code: `bimdf::TMesh` stays the QUANTIZATION view — arc lengths plus each arc's
  exact symbolic length over the seamless solver's promoted variables, which is
  meaningless outside the integer solve — and `TopologyLayout` is the GEOMETRIC
  and COMBINATORIAL view, with no solver variables, so singularity scoring,
  guides, symmetry, quality scoring and debug rendering can consume it without
  dragging the solver along. Node and arc ids are shared between the two.

  The tracer now optionally keeps the geometry it used to discard
  (`Charts::captureGeometry`): T-node positions at creation, separatrix
  polylines sliced out of the walk's trail by the same monotone curve parameter
  its events already carry, and crease/boundary polylines from the chain
  vertices. **Nothing in `solveBimdf` reads any of it**, so capture cannot move
  an assignment — verified byte-identical output with capture on versus off
  across all six corpus models.

  `validateTopologyLayout` checks the layout's combinatorial invariants and
  deliberately separates two failure classes, mirroring how the tracer already
  treats them. A HARD violation — a bad id, a non-finite position, an arc
  pointing at a missing node, an arc bounding no patch — means the graph is
  corrupt and nothing may consume it. A patch whose boundary walk does not close
  is LOCAL: it is reported by id and its arcs are excluded, exactly like a
  rejected orbit, and the sound remainder proceeds. All six corpus models
  validate; rocker-arm carries one contained non-closing patch out of 199, which
  is a Phase B target.

  Reachable behind `CYBER_ZR_LAYOUT` (`=1` reports the layout statistics and the
  validation verdict; any other value is a path prefix and also writes
  `<prefix>.json` and `<prefix>.obj`, both byte-reproducible). The public
  `zremesher` quad method that will carry this without an env var is later in
  the same change. `examples/21_topology_layout.py` renders the layout's arcs
  and singularities over the quads they produced.

- **Exact symmetry (`--symmetry x|y|z`) — mirrored CONNECTIVITY.** For
  retopology the requirement is `left topology == mirrored right topology`, not
  `left shape ~= right shape`. The second is what remeshing a symmetric model
  and hoping gives you: a field solve on a symmetric surface is not a symmetric
  function of it, so cones land wherever iteration order and floating-point ties
  put them and the halves come back with different edge counts.

  So it is obtained by construction: cut at the midplane, solve one half, mirror
  the connectivity, weld the centerline. Matching every vertex and face to its
  reflection, spot goes from 1377 unmatched vertices and 1661 unmatched faces to
  **0 and 0**; cheburashka from 1590 and 1698 to **0 and 0**.

  Three pieces, each with a failure mode that had to be handled. The split clips
  faces the plane passes through, so the border is on the plane rather than
  ragged. The border is then projected back onto the plane after remeshing —
  necessary because the extraction's border does NOT land on the cut, it ends
  where the isolines end and drifts up to ~3 edge lengths, which no distance
  tolerance can absorb; projecting every border vertex is correct precisely
  because the input was closed, so the half's only border IS the cut. Faces the
  projection flattens into the plane are membranes that would sit inside the
  model as an internal wall, and are removed.

  `--target-quads` names the whole model, so the half is solved for half of it.
  Known residue: cheburashka comes back with 3 boundary and 2 non-manifold edges
  where a membrane was removed; spot and stanford-bunny are clean.

  The plane math moved from `retopo` to `cyber/core/plane.hpp` so the remesher
  and the interactive tools share one definition without either depending on the
  other; `retopo::Plane` and friends are re-exported, so every existing caller
  is unchanged.

- **Topology guides — a stroke that becomes an edge loop, not just a field
  bias.** A flow guide has always been a soft request: the cross field is biased
  toward the stroke, the loops nearby lean that way, and none of them is pinned
  to it. That is right for steering flow and wrong for "put a loop exactly HERE",
  which is most of what a retopology artist draws — an eye, a mouth, a shoulder,
  a knee.

  A guide now carries a mode. `orientation` is the old behaviour and is the
  default, so every guide authored before this field existed is unchanged.
  `topology` says the stroke is meant to become a curve in the mesh.

  The mechanism reuses creases rather than growing a parallel one: a pinned
  crease already IS "run an edge loop along this curve", so a topology guide is
  projected onto the solve mesh as a connected edge path and handed to that
  machinery. Projection snaps each guide point to its nearest vertex and then
  JOINS consecutive snaps by shortest edge paths — joining is what makes the
  result connected, since a stroke sampled more coarsely than the mesh would
  otherwise leave gaps no edge chain can follow. It is deterministic, it reports
  how far it had to stray, and it declines rather than returning a broken path.
  A guide that cannot be projected is reported by name, never dropped.

  Measured on the RESULT, as the fraction of the guide with an output edge both
  near it and ALIGNED with it:

  | fixture | orientation | topology |
  |---|---|---|
  | sphere equator | 51.6% | **87.5%** |
  | sphere loop tilted 35° | 52.3% | **86.7%** |

  Requiring alignment is what makes that number honest. Edges crossing the guide
  at right angles are everywhere in a dense mesh, and counting them scored the
  ignore-the-stroke case above 80%.

  Reachable from the CLI sidecar today (`"mode": "topology"`, `"closed": true`);
  an unrecognised mode is an error rather than a silent fallback.
  `examples/24_topology_guides.py` renders it.

- **The ZRemesher surface reaches the bindings, not just the CLI.** The public
  method shipped everywhere; its parameters did not. Quality mode, symmetry and
  guide mode were CLI-only, and the layout statistics and the selected candidate
  went to stderr and nowhere else — so neither of the engine-bindings spec's
  scenarios could even be written. The C ABI gains `CyberZRemesherParams`,
  `CyberZRemesherReport`, `CyberFlowGuideEx` / `CyberGuidanceEx`,
  `cyber_default_zremesher_params`, `cyber_remesh_zremesher` (the parity entry
  point) and `cyber_remesh_guided_ex` (mode-bearing guides on *any* quad method,
  because the CLI can attach one to any method).

  By SIBLINGS, never by new fields on shipped structs: `CyberRemeshParams` and
  `CyberFlowGuide` are passed as arrays that callers stride by `sizeof`, so a
  new field misreads every already-compiled caller's array — the same reasoning
  the header already records for `cyber_retopo_subdivide_ex`.

  Python mirrors all of it: `ZRemesherParams`, `ZRemesherReport`,
  `FlowGuide.mode` / `.closed`, and `remesh(..., zremesher=...)` attaching
  `Mesh.zremesher_report`. Unknown values are **rejected, never reinterpreted** —
  a symmetry axis quietly clamped to `"none"` hands back an asymmetric mesh for
  a symmetry request, and a typo'd `"topolgy"` quietly biases the field instead
  of cutting a loop.

  Two supporting changes made this one implementation rather than two:
  `remeshSymmetric()` lifts the split / border-snap / mirror / recount sequence
  out of `apps/cli/main.cpp` into `symmetry_layout` so the CLI and the ABI run
  the same code, and `ZRemesherRunReport` / `LayoutRunReport` are caller-owned
  sinks on the options structs (null keeps the historical stderr-only behavior
  exactly).

### Fixed

- **Guidance counts were bounded after the pointer arithmetic, not before.**
  `toGuidance` built its density range as `src + count` and relied on the
  allocator to refuse an impossible request — but a count a binding marshalled
  from a signed `-1` arrives as `SIZE_MAX`, and `src + SIZE_MAX` is undefined
  behavior in its own right, so the allocator never got the chance. The ABI
  already returned `CYBER_ERR_INVALID_ARG`; it reached that answer *through*
  UB, which UBSan reported on the sanitizer lane. Now bounded against
  `max_size()` first. Observable behavior unchanged.

- **`cyber_remesh` with `CYBER_QUAD_ZREMESHER` ignored `adaptivity`.** It left
  the ZRemesher option at its `0.0` default while the CLI has always forwarded
  the parameter, and the default is `1.0` — so the same request produced a
  different mesh from Python than from the CLI. `cyber_remesh_zremesher`
  forwards it and Python routes `quad_method="zremesher"` through that entry
  point. Regression test: `python_test_zremesher_bindings` runs both surfaces on
  a **torus** and compares counts. The fixture is not a sphere on purpose —
  constant curvature is sized identically at adaptivity 0 and 1, so a sphere
  passes whether or not the parameter is forwarded (verified by mutation: torus
  CLI 696v vs Python 698v; sphere identical either way).

- **A public `zremesher` quad method.** The topology-layout work is no longer
  reachable only through environment variables: `--quad-method zremesher` on the
  CLI, `CYBER_QUAD_ZREMESHER` (4) on the C ABI, `quad_method="zremesher"` from
  Python. `quad-cover` remains the default and is untouched.

  It is not a new algorithm — structurally it is the quad-cover path, same cross
  field, same seamless solve, same isoline extraction, with the topology-layout
  stage on and the tracing options **the layout** wants rather than the ones the
  shipped quantizer's guided rounding wants. That distinction is why it has to
  be a method rather than another flag: Phase B measured that boundary chains fix
  most of the Stanford bunny's abandoned separatrix launches, but `quad-cover`
  cannot turn them on, because the recovered regions reshape the flow its guided
  rounding is tuned against. `zremesher` does not use that rounding, so for it
  the trade does not exist.

  `SeamlessLayoutOptions` is what carries it: the tracer no longer reads the
  environment to decide what to trace, the caller does. It always routes to the
  native seamless solver, since the layout is traced from that solver's map, and
  it takes the same field-aligned per-island fallback `quad-cover` does.

  Measured, each lever toggled independently on the corpus: boundary chains take
  the bunny's abandoned launches 4 → 2 and recover 12 more patches at a flat
  rejection ratio, leaving every closed model bit-identical; fold repair cuts
  fold-damaged node rotations (bunny 17 → 11, rocker-arm 17 → 16, cheburashka
  3 → 1) with rejections unchanged. Both are coverage and repair wins, and
  **neither moves the fold-robustness gate**.

  The run report now names the EFFECTIVE quad method — after any
  build-capability fallback — which it did not carry at all before.

- **Layout-containment diagnostics, and two fold-repair levers** (Phase B of the
  same change, in progress). The tracer reported *that* it contained a region
  but never *why*, which made the fold-robustness work guesswork. It now reports
  the rejection reason per orbit (sector winding / corner count / side mismatch /
  abandoned cone), the degradation site per node (boundary fan / unanchored end /
  T-node winding / T-node interior / fan reclassification) and how many nodes are
  genuinely unseatable — winding greater than twice their arc ends, so no
  in-range sector assignment exists at all.

  `examples/22_layout_robustness.py` is the artifact those numbers come from: it
  sweeps the corpus against target counts and adaptivity and prints what it
  measures, met or not. Baseline at 2000 quads: 178 rejected orbits, 1
  non-closing patch, 57 abandoned launches; reasons 88 sector winding, 52 corner
  count, 9 side mismatch, 29 abandoned cone.

  Behind `CYBER_ZR_FOLD_REPAIR`, two levers that are strictly better estimates
  and output-neutral on the corpus: a **feasible-rotation projection** (the
  [1,2] corner/pass-through range is what a rotation system around a node
  requires, so when the winding admits any in-range assignment the closest one
  beats an out-of-range rounding) and a corrected **winding lift target** (QEx
  Algorithm 8's lift targeted the number of incident arc ends, which undercounts
  badly at a negative-index cone — a valence-5 cone with two surviving ends
  lifted to 2 instead of 5). Measured: degraded nodes rocker-arm 17 → 13 and
  bunny 11 → 5, reclassification failures rocker-arm 15 → 11 and bunny 8 → 2,
  **rejected orbits unchanged** and output byte-identical. The Phase B gate is
  not met.

  Recorded as refuted, with numbers, so it is not retried: overriding the
  developed winding with the topological one made things clearly worse (rejected
  orbits bunny 47 → 69, cheburashka 12 → 23). The measurements point the next
  lever upstream — the residual is an index/geometry disagreement at
  negative-index cones, and 37 of the bunny's 57 abandoned launches are open
  boundaries, not fold damage at all.


## [0.7.0] - 2026-08-25

### Added

- **Smooth (Catmull-Clark) subdivision.** `cyber_retopo_subdivide` was linear
  only — the header said so outright — so the sole way to recover curvature was
  reprojecting onto a Target, which requires still having one. The smooth rules
  (face points, edge points, and the `(F + 2R + (n-3)P) / n` vertex weighting)
  are now available as an opt-in mode: `Subdivision.CATMULL_CLARK` for
  `Mesh.subdivide` in Python, `cyber_retopo_subdivide_ex` in C, and
  `retopo::subdivide(mesh, SubdivisionMode::CatmullClark)` in C++. A cube's
  worst dihedral falls 90° → 43° → 26° → 14° → 7° across four levels where the
  linear path stays at 90° forever.

  Creases are held sharp by the same "feature edge or boundary" predicate the
  pipeline's quad relax freezes vertices on, so an open patch keeps its border
  and corners instead of shrinking inward, and a fully feature-tagged cube comes
  back an exact cube. Tag the edges you want kept hard before subdividing.

  **Linear remains the default and is unchanged bit-for-bit** — pinned by a test
  hashing the float bits of the linear output against values captured before the
  change. The mode arrived as a sibling C entry point rather than a new parameter
  on `cyber_retopo_subdivide`, so already-compiled callers of the ABI keep
  working; the mode-less name is now a forwarder to the linear case.

- **Loop subdivision — triangle in, triangle out.** `cyber_retopo_subdivide`
  applies Catmull-Clark topology, so a triangle mesh came back as quads (three
  per triangle) and there was no way at all to get a denser *triangle* mesh.
  `cyber::retopo::loopSubdivide`, `cyber_retopo_loop_subdivide` and
  `Mesh.loop_subdivide` split every triangle into four with the standard Loop
  weights, including the boundary rules that keep an open mesh's border where it
  is instead of pulling it inward.

  Two modes, and the caller always names one: `SMOOTH` uses the true Loop
  weights and **changes the shape** (the surface converges to the Loop limit
  surface), while `LINEAR` is a pure 1-to-4 split that leaves every original
  vertex exactly where it was — "more polygons, same shape". Nothing infers the
  mode, so smoothing is never silent.

  A face that is not a triangle is refused, naming the face and its side count,
  rather than being fan-triangulated behind the caller's back. `Mesh.triangulate`
  is the explicit opt-in and is now exposed to the bindings too.

- `CyberStatus` gains `CYBER_ERR_UNSUPPORTED_TOPOLOGY` (value 8, appended — every
  existing value keeps its number): the mesh is well-formed but its topology is
  not what the operation is defined on. Python raises the matching
  `UnsupportedTopologyError`; Swift maps it to `.unsupportedTopology`.

## 0.6.0 - 2026-08-24

### Upgrade notes

Everything a consumer can notice, in one place. Each item is explained where it
is listed; this is the index, not the account.

**Different output from the same call**

- `sharpEdgeDegrees` reaches the engine from the C ABI and the bindings for the
  first time (it ran at 40° regardless before) — pass `40.0` to reproduce the old
  topology. *(Hardened)*
- Every AO bake produces different pixels, and `aoSamples` defaults to 64 rather
  than 16 — pass `--ao-samples 16` for the old budget. *(Fixed)*
- UV seam routing now follows convex creases; `convex_weight = 1.0` restores the
  old routes. *(Fixed)*
- The native seamless solve pins lattice-dependent seam translations, changing
  crease-heavy output (fandisk improves at every density). *(Fixed)*
- Meshes at coordinates far from the origin now terminate under a resolution
  floor and a face budget instead of exhausting memory; meshes at ordinary
  coordinates are byte-identical. *(Hardened)*
- A mesh with *any* sharp features now routes to the native seamless solver
  first; only a surface with none at all (a sphere, a torus) goes to the vendored
  Geogram field. The old threshold sent every real scanned and CAD model to the
  vendored path. Across eight community models this moves mean median quad angle
  77.4° → 81.0° and the irregular-vertex rate 6.4% → 3.4%, at the cost of
  rocker-arm (82° → 74°). `CYBER_QC_ROUTE_CREASE=0.02` restores the old
  behaviour. *(Fixed)*
- The cross-field smoother converges instead of oscillating, so remesh output
  changes on every input. It ran Jacobi on a bipartite dual graph and two-cycled:
  on a flat grid 15 of 72 faces came out at the maximum 45° off, with the sweep
  count's parity deciding which half of each quad was damaged. *(Fixed)*
- Remeshing is now reproducible across toolchains, and the meshes it produces
  change as a result. The cross field no longer routes angles through libm's
  `sinf`/`cosf` (Apple Clang and GCC disagree on those by one ULP), and the
  isoline graph is traced in index order rather than hash order. Builds from
  different compilers previously traced the same open paraboloid into 144 or 224
  quads; they now agree exactly. *(Fixed)*
- The CLI clamps `--sharp-edge` and `--adaptivity` to their documented ranges
  instead of warning that it clamped them and then running the raw value, so a
  `--sharp-edge` below 30° now produces different topology than it used to.
  *(Hardened)*

**Different numbers in a report**

- `AtlasResult::packedArea` is real geometry coverage, not the bounding-box
  fraction (which is now `packedBoxArea`); `chartCount` excludes degenerate
  islands, counted separately as `droppedCharts`. *(Fixed)*
- `CyberSoftTransformReport.moved` counts distinct vertices for the weighted
  relax too. *(Changed)*
- `elapsedSeconds` now includes export. *(Changed)*

**Different API behaviour**

- Failures that used to escape as C++ exceptions (`nlohmann::type_error`,
  `std::bad_alloc` from an oversized PNG, a solver throw through
  `cyber_remesh_guided`) are typed statuses. *(Hardened)*
- Export presets can no longer name a file outside the output directory.
  *(Hardened)*
- A preset whose maps do not all expand to distinct file names is refused when it
  loads, naming both offenders, instead of writing one file and reporting two.
  *(Hardened)*
- Writes report failure when the flush fails, instead of after a buffered write.
  *(Hardened)*
- `CyberBakeParams` gained a trailing field: recompile clients, and always
  initialise via `cyber_default_bake_params`. *(Changed)*
- `cyber_mesh_edge_faces` returns the number of entries WRITTEN (at most 2, the
  size its own prototype declares), not the edge's true face valence; read the
  valence through the new `cyber_mesh_edge_face_count`. *(Hardened)*
- The automatic UV atlas is interruptible: `uv::unwrapAtlas` and `uv::autoSeams`
  take a trailing `ProgressSink*` / `const CancelToken*` (both defaulted, so
  existing calls compile and behave unchanged), the new
  `cyber_uv_atlas_cancellable` exposes them over the C ABI, and
  `exportbundle::writeBundle` finally forwards its own token into the unwrap it
  performs on a UV-less low-poly — the phase that dominated the call was the one
  the advertised `CYBER_ERR_CANCELLED` could not reach. A cancel leaves the mesh
  untouched. `AtlasResult` gained a `cancelled` flag. *(Hardened)*
- `IBackend::closestPointsBvh` / `raycastBvh` take the `FlatBvh` snapshot instead
  of loose node/triangle pointers, so a device backend can tell which snapshot it
  is holding; a custom backend needs recompiling against the new signature.
  *(Hardened)*

**Different environment**

- The shared library exports only `cyber_*`; the vendored third-party symbols are
  no longer visible. *(Changed / Hardened)*
- The engine no longer touches the host's signal, terminate and new handlers,
  `LC_NUMERIC`, or the `std::cout`/`std::cerr` buffers. *(Hardened)*
- Documents carry a new optional attribute section (the format version is
  deliberately unchanged, so both directions still load). *(Hardened)*
- New environment variable `CYBER_BACKEND`; the Python loader no longer adopts a
  build tree it does not own. *(Hardened)*

### Added

- **Isotropic (triangle) remeshing in the bindings.** `isotropicRemesh` — the
  engine's real density control (target edge length, iterations, curvature
  adaptivity, PN-triangle smoothing) — had no C entry point at all: it ran only
  as an internal stage of the quad pipeline, so no binding consumer could
  densify or re-tessellate a mesh. `cyber_mesh_isotropic_remesh` (Python
  `Mesh.isotropic_remesh`) remeshes a handle in place to a target edge length,
  and does for the caller what the C++ contract makes them do themselves: it
  triangulates a non-triangulated input rather than rejecting it, tags feature
  edges, and freezes the projection reference from the input surface before any
  pass runs. `CyberIsotropicParams` / `IsotropicParams` mirror the engine's
  options, filled by `cyber_default_isotropic_params` — except the target edge
  length, which is world-space and is left at 0 and rejected rather than
  invented. Painted density is deliberately not mirrored yet and the header says
  so, naming `cyber_remesh_guided` as where it is reachable. The result is
  always a triangle mesh, and every element id is invalidated.

- **The benchmark regression gate actually runs.** `tests/CMakeLists.txt` only
  registered the `bench` case when python3 could import numpy and scipy, and no
  CI lane installed either, so the case never appeared in the ctest list at all —
  a green suite with no `bench` line reads exactly like one with a passing bench
  line. The case is registered unconditionally now (it reports CTest's SKIP code
  when it cannot run, so it stays visible), and a nightly `hardening.yml` job
  builds with the vendored field REQUIRED and fails if the gate reports SKIPPED.

- **Seam-driven UV unwrap in the bindings.** The manual UV workflow used to
  dead-end: the C ABI shipped 24 seam entry points — `cyber_seam_set_*` and the
  eighteen `cyber_seam_path_*` routing functions — and nothing that consumed
  them. `unwrapIslandToUv`, `packIslands` and `stitchAlongSeams` were C++-only,
  and the one reachable unwrap, `cyber_uv_atlas`, computes its own cuts and
  ignores whatever the caller marked. `cyber_uv_unwrap_seams` (Python
  `Mesh.unwrap_seams`) takes the seam set you built, cuts the mesh into islands
  at exactly those edges, unwraps, re-orients and packs them, and reports through
  the existing `CyberAtlasResult`; a cancellable twin observes cancellation
  before any UV is written. An empty seam set means *do not cut*, never *seam it
  automatically*, and a null one is a typed argument error rather than a silent
  fall back. `cyber_uv_stitch_seams` (`Mesh.stitch_seams`) is the inverse — the
  "sew" half of the model the seam set documents.
- **`Mesh.edge_count()` and `Mesh.edge_between()` in Python.** Marking a seam
  takes an edge id, and there was no way to enumerate or look one up from the
  binding — `cyber_mesh_edge_count` existed in the C ABI but was never declared
  on the Python side.

- **FBX import.** `.fbx` joins OBJ / PLY / STL / glTF in the import dispatch,
  via the vendored [ufbx](https://github.com/ufbx/ufbx) (MIT, `thirdparty/ufbx`).
  Reads polygons at their authored arity, per-corner UVs and normals, and vertex
  colors; applies each mesh node's world transform so multi-node scenes arrive
  assembled; and normalizes the file's axis and unit conventions, so a Z-up
  centimetre export lands at the same size and orientation as the OBJ of the same
  model. Animation, skinning, materials, cameras and lights are ignored, not
  rejected. FBX is **import-only** — writing it needs the proprietary binary
  container, which no permissively licensed library produces — and an export to
  `.fbx` now fails with an error naming the writable formats instead of a bare
  "unsupported format".
- **`Mesh.subdivide()` in the Python binding.** Linear subdivision (Catmull-Clark
  topology, no smoothing) was reachable from C++ and the C ABI but not from
  Python. `mesh.subdivide()` splits every n-gon into n quads in place and returns
  the new face count; `mesh.subdivide(project_to=other)` additionally projects
  every vertex onto `other`'s surface, which is what recovers curvature that
  linear subdivision alone cannot add. See `examples/16_subdivide.py`.
- **`cyber_mesh_load` / `cyber_mesh_save`** in the C ABI, and `Mesh.load` /
  `Mesh.save` in Python: the canonical names for behaviour that has dispatched on
  the file extension since PLY/STL/glTF landed. `cyber_mesh_load_obj` /
  `cyber_mesh_save_obj` and `Mesh.load_obj` / `Mesh.save_obj` remain as aliases.
- **FBX joins the fuzzed import surface.** `.fbx` is dispatched by
  `fuzz_mesh_io`, with seeds committed so `fuzz_corpus_replay` exercises it on
  every build, and the ufbx allocators are bounded so a forged header cannot
  commit memory the stream has no bytes for — it comes back as a typed error like
  any other malformed input.
- **Python coverage for the bindings and the example gallery.**
  `test_io_formats.py` (round-trip across every writable format, FBX load, FBX
  export refusal, unknown-extension error, alias behaviour), `test_subdivide.py`
  (face-count growth, n-gon splitting, reprojection landing vertices on the
  target, empty-mesh error), and `test_examples.py`, which executes the six
  offline examples end-to-end — nothing ran them before. All three are registered
  with CTest under the existing capability-gated convention.
- **The mobile targets build, and 64-bit ARM is now executed in CI.** Three
  things blocked the platform the product actually ships on, and nothing
  reported any of them. The `android` preset had **never configured**: it set
  `CYBER_ENABLE_OPENCL=ON` and `src/accel/CMakeLists.txt` asked for
  `find_package(OpenCL REQUIRED)`, while the NDK ships no OpenCL — the
  configure died at that line before reaching any other setting in the preset.
  `apps/cli/main.cpp` could not compile for either mobile target, because its
  number parser instantiated `std::from_chars` for `float` and libc++ (the
  standard library of both Apple platforms and the NDK) declares those
  overloads **deleted**. And no lane ever ran the engine on aarch64, so no lane
  could see a defect that depends on the mobile word size, alignment, `char`
  signedness or floating-point contraction. Now: OpenCL is off for Android and
  the `REQUIRED` is a message that names the NDK and the option that disables
  it; both mobile presets drop the POSIX-socket network bridge an app has no
  use for and their `displayName`s say what they really enable; the parser
  keeps `std::from_chars` where it exists and reimposes its exact grammar over
  `strtof`/`strtod` where it does not (`apps/cli/parse_number.hpp`, with
  `tests/cli/test_parse_number.cpp` holding the two paths to the same
  verdicts); and three CI lanes join the set — `arm64-cross` (cross-compiles
  everything for aarch64 and **runs the suite** under `qemu-user`, via the new
  `linux-arm64-cross` preset), `android-ndk` (configures and builds the preset
  with the runner's NDK, CLI and suite included), and a reporting `ios-preset`.
- **The Metal backend gets ARC, and stops being the one file nobody compiles.**
  `cyber_accel_metal` was added without `enable_language(OBJCXX)`, so CMake fed
  the `.mm` to the plain C++ driver and it never got ARC — every `newCommandQueue`,
  `newBufferWith…`, `newFunctionWithName`, `newComputePipelineState…` and
  `newLibraryWithSource` result leaked, on every dispatch. The target now
  declares the language and compiles with `-fobjc-arc`. Separately, a new
  ctest case `metal_objcxx_syntax` parses `metal_backend.mm` on **any** host —
  clang handles Objective-C++ with `-fobjc-runtime=ios-13.0`, and
  `tests/packaging/objcxx_stubs/` supplies declaration-only Metal/Foundation
  headers with the SDK's exact signatures — under `-Wall -Wextra -Werror`. It
  mutation-checks itself on every run: a deliberately mistyped selector must be
  rejected, or the gate reports that it proves nothing.
- **A host can now bound the engine's worker threads.** Both parallel loops
  (the pipeline's per-vertex relax and accel's `IBackend::parallelFor`) read
  `std::thread::hardware_concurrency()` directly, so an AO bake took every core
  of a machine an interactive host was trying to share, and there was no
  in-process way to ask for less. `cyber::setMaxWorkerThreads` /
  `maxWorkerThreads` (`cyber/core/threading.hpp`) and the C ABI's
  `cyber_set_max_worker_threads` / `cyber_max_worker_threads` cap it; 0 keeps
  the old behaviour. It is an API and not an environment variable because a
  host learns its budget while already running and multi-threaded. The cap
  changes speed and CPU load only — the loops split a range into contiguous
  chunks and each chunk writes only its own indices — and
  `tests/core/test_threading.cpp` pins that by comparing a capped remesh
  against an uncapped one vertex for vertex. The CPU backend's device name now
  reports the effective count rather than the machine's.
- **Named soft-selection slots now persist with the document, end to end.**
  The two halves existed and nothing connected them: `cyber::app::Document`
  serialized a `softSelections` map and the C ABI kept its own slot map on
  `CyberMesh`, but no code in the tree moved weights between them in either
  direction — a slot saved through the ABI was absent from the saved document,
  and a slot in a loaded document was invisible to the ABI, so the
  manual-retopology spec's "savable to and loadable from named slots persisted
  with the document" was unreachable. The seam is now a real document handle:
  13 additive `cyber_document_*` entry points (`_create` / `_free`,
  `_set_target` / `_set_edit_mesh`, `_target` / `_edit_mesh`, `_slot_count` /
  `_slot_name` / `_slot_weights`, `_save` / `_load` / `_save_file` /
  `_load_file`), exposed as `cyberremesh.Document` in Python and
  `Document.swift` in Swift. `set_edit_mesh` copies the handle's named slots
  into the document and `edit_mesh()` hands back a fresh handle carrying them,
  so a weighted selection survives serialize → drop every handle → load. The
  seam is BY VALUE and explicit: nothing is aliased or kept in sync, and the
  live (unnamed) weight field is deliberately not persisted. Implemented only
  when the application-shell library is in the build (`-DCYBER_BUILD_APP=ON`,
  the default); without it the symbols still exist so the ABI is stable and
  every call reports the missing module, exactly like the UV handles.

- **`cyber_retopo_selection_transform_pinned`** — the weighted transform can
  finally honor pins from the C ABI, Python and Swift. See *Fixed*.

- **Sculpt handoff bridge** (openspec change `add-claycore-bridge`): the
  receiving half of a `sculpt -> retopo -> UV -> bake` pipeline, landed with
  **zero hard dependency on any sculpting or volumetric engine**. Three
  additive pieces, all reachable from C++, the C ABI and Python:

  - **Versioned sculpt handoff ingest** (`src/handoff`, `cyber::handoff`). A
    triangle mesh with positions, per-vertex normals, vertex colors and a
    `material_mix` weight arrives as a Target through a file (PLY profile, or
    `.gltf`/`.glb` declaring `asset.extras.cyberSculptHandoff` — that route
    carries the version, the producer label, geometry, vertex colors and
    per-vertex normals; `material_mix` is the one payload glTF cannot express
    and its absence is warned about, not passed over), a stream (stdin), or
    plain in-memory buffers
    with no intermediate file. The version gate runs on the header text **before any
    geometry is read**, so a bad version can never produce a partial Target,
    however malformed the rest of the payload is; the rejection is the existing
    typed `io::ErrorCode::IncompatibleVersion` (`CYBER_ERR_INCOMPATIBLE_VERSION`
    on the C ABI, `IncompatibleVersionError` in Python) and names both the
    version found and the version supported. A newer *minor* is refused rather
    than read with the unknown parts dropped, matching the export-preset
    unknown-field precedent. A triangle the mesh refuses (a repeated vertex
    index) is counted and warned about — `Handoff::droppedFaces`,
    `CyberHandoffInfo::droppedFaces`, `HandoffInfo.dropped_faces`,
    `handoff.droppedFaces` in the CLI report — rather than dropped in silence.
    CLI: `--target <path|->`, mutually exclusive with `--input`, plus a
    `handoff` block in the JSON report and `--bake <csv>` to override the
    preset's map set.
  - **Field-sampled baking through an evaluator interface**
    (`cyber::bake::FieldEvaluator`, `BakeParams::field`, `cyber_bake_field`,
    `cyberremesh.FieldEvaluator` / `bake_field`). A pure-abstract interface —
    signed distance, gradient, occlusion, plus a free central-difference
    curvature default — is the only coupling point to a volumetric engine. With
    one attached, normal / AO / curvature / cavity sphere-trace the cage ray
    through the field and read exact gradients, and the Target mesh becomes
    optional for those four maps. **Without one, output is bit-identical to the
    pre-bridge raycast implementation**, pinned by FNV checksums over all seven
    maps captured from the unmodified binary before the refactor.
  - **Conform** (`cyber::retopo::conform`, `cyber_conform`,
    `cyberremesh.conform`). Re-snaps an EditMesh onto a replaced Target,
    preserving topology exactly (only `setPosition` is ever called), and reports
    the **maximum AND RMS** deviation plus every vertex past a caller-set
    threshold. It completes and flags rather than silently stretching. The same
    code now backs the manual-retopology spec's whole-mesh "snap all vertices to
    Target" command (`retopo::snapAll`), which had never been implemented, so
    the two cannot diverge.

  Format: `docs/sculpt-handoff-format.md`. Demo: `examples/18_sculpt_handoff.py`
  (a synthetic producer plus the one-command run, from a file and over a pipe).

  **Two honest gaps.** (1) The handoff format is defined **unilaterally by this
  repository**. The proposal's task 1 called for one shared document agreed with
  ClayCore's export-profile change; ClayCore is not in this tree and no
  negotiation took place, so the versioning is proven only against files this
  repo writes — "loud on both sides" is demonstrated on our side only. (2) No
  real volumetric evaluator was exercised. The field path is validated against
  analytic SDF test doubles (a sphere and a plane), which proves the interface
  and the sampling math but not interoperability with any actual sculpting
  engine.

- **Flow guides and painted density** (openspec change `add-flow-guides`): two
  explicit, opt-in *local* controls on an otherwise fully global pipeline.
  **Flow guides** are 3D polylines drawn on or near the Target whose tangent the
  4-RoSy cross field is softly biased toward within a per-guide influence radius
  and strength. **Painted density** is a per-vertex or per-face scalar on the
  Target that multiplies local sizing under the documented relation
  `localEdgeLength = baseEdgeLength / sqrt(density)` — density is a
  quads-per-unit-area multiplier, so it composes with the area-derived
  `targetQuadCount` instead of fighting it. Nothing here is inferred; this is
  deliberately distinct from the automatic curvature adaptivity the roadmap
  descoped after measurement retracted its claimed win.

  **Unguided output is byte-identical**, structurally rather than by tuning:
  every guidance path sits behind a null/empty check, so a run without guidance
  executes textually the same arithmetic as before. Verified by bit-comparing
  `CrossField::real/imag`, by two in-process `remesh()` runs compared as raw
  float bits plus face lists, by a CLI `cmp` of an empty-guide-list run against
  no `--guides` at all, by `ctest -R bench` matching its recorded baseline
  number for number, and by re-running `fandisk` at 1000 and 4000 quads
  byte-for-byte against a pre-change baseline. `spot` and `stanford-bunny`
  could NOT serve as oracles: on the vendored Geogram quad-cover route they are
  already run-to-run non-deterministic on unmodified `main` (6 identical spot
  invocations produced 6 distinct outputs there), which is a pre-existing
  property of that route and is recorded here rather than papered over.

  **A density of 1.0 everywhere is a no-op too.** 1.0 is the identity of the
  sizing relation, but merely *carrying* a density field forces the native
  seamless route, so an all-1.0 paint used to change the mesh on the shipped
  default backend (`fandisk --target-quads 1000`: md5 `3add9afb` unguided vs
  `b390b987` with `perVertex=[1.0]*6475`). `validateGuidance` now drops a
  post-clamp neutral density and reports it as a non-fatal issue, so it is
  byte-identical to no density at all; a uniform density of any other value
  still applies normally.

  **Guidance is honored loudly or rejected loudly.** `IQuadrangulator` grows
  `acceptGuidance`, whose default body *declines* and states why, plus an
  `unhonoredGuidance()` query for guidance accepted up front but lost at run
  time. `PipelineResult::islandGuidance` carries one row per island naming the
  guides in range, whether guides and density were honored, and the reason if
  not. Supplying guidance **forces the native seamless route** on the
  quad-cover backend, because the vendored Geogram solve has no hook for either
  input — a documented quality trade (native ~4-5% irregular vs vendored 1-4%
  on smooth organics), not a hidden one. Two ways guidance could still slip
  through that audit are closed: the developer kill switch `CYBER_QC_NO_NATIVE`
  disables the native solve, and a guided island then takes the vendored route —
  it is now reported as unhonored, naming the env var, instead of being counted
  as honored; and a guide whose reached faces are ALL owned by hard pins (feature
  edges, boundaries, crease alignment) leaves the field bit-for-bit unguided, so
  it is now reported with its absorbed-face count rather than counted as honored
  (a partial absorption still counts as honored — the guide did move the field).
  Zero-radius guides, guides with fewer
  than two points, non-finite values and mismatched density array lengths are
  rejected as fatal rather than silently ignored; out-of-range strength and
  density values clamp with a reported effective value.

  Reachable from the C ABI (`cyber_remesh_guided` + `CyberWarningCb`;
  `cyber_remesh` and `CyberRemeshParams` are untouched and ABI-identical),
  Python (`FlowGuide`, `remesh(..., guides=, density=)`, warnings surfaced both
  as `Mesh.guidance_warnings` and through `warnings.warn`), the network bridge
  (`push_guides` / `pull_guides` / `clear_guides` / `push_density` /
  `pull_density`, no protocol-version bump), and the CLI (`--guides
  <file.json>` sidecar plus a `guidance` block in `--report`). Every sidecar
  field is type-checked before it is read — a string where a number belongs is
  an exit-2 argument error naming the file and the field, where it previously
  reached an unguarded JSON accessor and aborted the process (SIGABRT) — and
  `main` has a last-resort handler so no escaping exception can ever replace a
  diagnosis with a crash. See `docs/flow-guides.md`.

  **Exit gate: NOT met corpus-wide, reported as measured.** The proposal's gate
  is <=15 deg mean deviation between extracted loop direction and the guide
  tangent inside the influence radius (random baseline 22.5). Measured by
  `examples/17_flow_guides.py` on `spot` and `stanford-bunny` at 2000 quads
  across three influence radii: guided **17.17 deg mean, 25.10 deg worst**, with
  2 of 6 runs at or under 15. Guides improve alignment on **every** run
  (e.g. spot 30.01 -> 18.17, bunny 15.40 -> 11.31), and the constraint is strong
  at the field stage (a flat grid with a 45-deg guide goes 44.84 -> 11.29 deg,
  and a tube end-to-end through the field-aligned backend 12.08 -> 9.23). The
  loss is dilution between the cross field and the extracted mesh — the seamless
  Poisson solve, integer rounding and isoline extraction each shed angular
  fidelity. Carrying the guide into the seamless solve as a per-face
  target-frame rotation is the follow-up.

- **Auto-routed seam paths** (openspec change `add-seam-path-tool`): the UV
  Path tool. Place waypoints on the EditMesh and the engine routes a least-cost
  edge path between consecutive ones, under a cost model that discounts
  feature-tagged edges and concave (valley) edges past a crease angle — so a
  two-click hop **follows the groove** instead of cutting the geodesic shortcut
  across flat surface, which is what makes spiral-looped auto-retopo output
  seamable at all.

  **The pending path stays editable until commit.** Any waypoint can be
  repositioned (by id, or dragged onto the nearest vertex within a radius) or
  deleted, and an edit re-routes **only the segments adjacent to it**; every
  other segment keeps both its route and its per-segment `routeRevision`
  counter, so a viewport can redraw exactly what changed. Committing marks the
  route into the existing `cyber::uv::SeamSet` — the one seam model, the same
  `mark`/`erase`/`sew` set a hand-drawn seam edits, so `computeIslands`,
  `unwrapIslandToUv` and `stitchAlongSeams` treat a routed seam exactly like a
  hand-marked one (all three are C++ only: the bindings expose the seam set and
  the path, not island/unwrap/stitch over it, and there is no stroke→seam
  gesture entry point on any surface) — and arms a **resume marker** so
  the next waypoint continues from the last committed point. Commit returns a
  `SeamCommit` undo record listing only the edges it *newly* marked, so
  `revertCommit` restores the exact pre-commit state and edges that were
  already seams survive the undo; dropping the resume marker only forgets where
  to continue and never touches a committed seam. A path with an unroutable
  segment (disconnected components) commits nothing and stays pending for
  repair.

  Reachable from the C ABI (`cyber_seam_set_*` / `cyber_seam_path_*` plus
  `cyber_mesh_edge_signed_dihedral`, all additive — no existing struct layout
  changed), Python (`SeamSet`, `SeamPath`, `SeamCostParams`) and the Swift
  package. Internally this reuses the existing shortest-path machinery:
  `cyber::retopo::shortestVertexPath` was generalized into a templated
  `weightedVertexPath` and is now a one-line wrapper over it, so Path Distribute
  and `cyber_mesh_shortest_vertex_path` are behaviour-identical.

- **Soft selection for manual retopology** (openspec change
  `add-soft-selection`): a per-vertex weight field in [0,1] over the EditMesh,
  filled from three region sources — a **line gradient** (0 at the anchor, 1 at
  the end and 1 beyond it, with optional 15° angle snapping measured in the
  view plane), a **sphere** with an easing falloff, and **painted strokes**
  (pressure-accumulating, with a subtract mode and a one-call gesture route) —
  reshaped by clear / invert / expand / contract / smooth, and consumed by a
  **weighted transform** (translate, rotate, scale scaled per vertex by the
  weight) and a **weighted relax**.

  **Auto re-snap is part of the operation, not a cleanup pass.** The weighted
  transform and relax re-project the vertices they move onto the Target inside
  the same pass, so a taper or a pose never peels the retopo off the sculpt and
  no `snap_all` follow-up is needed — running one would also drag the vertices
  the selection deliberately left alone. Vertices at weight 0 are skipped
  entirely, so they are bit-identical afterwards; pinned vertices are skipped
  too wherever pins can be supplied, which through the C ABI today means the
  weighted relax only — `cyber_retopo_selection_transform` has no `pinned`
  argument yet, so on that call weight is the only thing that holds a vertex
  still.

  Reachable from the C ABI (`cyber_retopo_selection_*`, including the
  x/y/z/pressure stroke route), Python (`Mesh.select_line`, `paint_selection`,
  `transform_selection`, the new `Snapper` wrapper, …) and the Swift package.
  Named selection slots have both halves of their persistence in place but are
  **not yet persisted end to end**: `cyber::app::Document` gains an
  **append-only** section that is written only when slots exist, so documents
  saved by earlier builds still load and documents without slots keep the exact
  previous byte layout (`kFormatVersion` deliberately unchanged), while the C
  ABI's `cyber_retopo_selection_save`/`_load` slots live on the mesh handle for
  the session. Nothing connects the two yet — that is shell work, and until it
  exists a saved slot does not survive the process.

- **Global integer quantization for the seamless solve** (openspec change
  `bimdf-quantization`, now archived). The quad-cover path's per-seam greedy
  integer rounding gains an alternative backend: a motorcycle-graph / T-mesh
  decomposition of the seamless parameterization solved as a min-deviation flow
  over a bi-directed graph, after Heistermann et al. 2023. GPL discipline —
  derived from the papers alone; no quadwild/libsatsuma code was read or
  vendored.

  Ships **opt-in and off by default**, behind `CYBER_QC_BIMDF`:
  `report` runs it in A/B mode without injecting, `guided` steers the greedy
  schedule by attracting its re-solves toward a Bi-MDF solve, `force` injects
  directly. With the flag off the output is byte-exact against the previous
  binary.

  Where `guided` engages it is a real win: stanford-bunny@3000 singularities
  135 → 94 with ear irregulars 37 → 19 (beating QuadriFlow's 20), and spot@3000
  pure flow-loop length 762.9 → 1274. **The default was not flipped**, and the
  campaign's own gates were not met: nefertiti@4000 pure singularities reach 411
  against a ≤200 gate. The remaining wall is now quarter-density fold damage
  (1297 cones, 110 degraded nodes), not the tracer or the quantizer — the
  self-spiralling-separatrix tracer wall and the whole-T-mesh refusal were both
  fixed along the way, so every corpus mesh now builds and solves a T-mesh.

  Two design directions were measured and **falsified**, and are recorded so
  they are not retried: coordinate-wise floor/ceil steering (lost on every
  scoring variant) and parity-aware quantization (the parity classes are a
  coupled GF(2) system beyond the graphic T-join, and exact flow realization
  hurts precisely where injection is blocked).

- **Curvature and cavity map baking** (openspec change `add-curvature-bake`),
  the missing quarter of the standard pre-texture recipe (curvature +
  occlusion + normal). `BakeMap::Curvature` encodes the Target's signed mean
  curvature around mid-gray — convex bright, concave dark — and
  `BakeMap::Cavity` keeps concavity only, so flat and convex both read white
  and the map drops straight into a multiply slot. Both are single-channel and
  follow the same cage projection, cancellation and PNG/EXR output as every
  other map: a curvature bake registers texel-for-texel with the normal bake
  taken through the same cage.

  Curvature is estimated with the Meyer et al. cotangent operator over a mixed
  Voronoi area (`cyber/bake/curvature.hpp`), fan-triangulating n-gons exactly as
  the BVH does so the estimate matches what the rays actually hit. Boundary
  vertices, where the cotangent formula has no meaning, take the mean of their
  interior neighbours instead of reading as a spurious crease.

  The new `BakeParams::curvatureRange` sets the curvature magnitude that
  saturates the map; the default of `0` auto-fits it to the 95th percentile of
  `|curvature|` over the Target, which is scale-independent and keeps a single
  pinched vertex from flattening everything to mid-gray. Reachable as
  `CYBER_BAKE_CURVATURE` / `CYBER_BAKE_CAVITY` in the C ABI and
  `BakeMap.CURVATURE` / `BakeMap.CAVITY` in Python.

- **Per-DCC export presets** (openspec change `add-export-presets`). `--preset
  <name|path>` turns a CLI run into a ready-to-hand-off bundle: remesh → auto-UV
  (when the low-poly has none) → bake the preset's map set → write the mesh and
  its maps under the preset's naming, color-space and normal-map conventions.
  Four built-ins ship — `blender`, `unity`, `unreal`, `gltf-generic`, listed by
  `--list-presets`. They differ where the target apps do: only `unreal` uses the
  DirectX green-channel convention, and only `gltf-generic` omits curvature
  (glTF 2.0 core has no slot for it). Color is the one sRGB map everywhere else;
  the data maps stay linear.

  A preset is versioned JSON (`cyber/core/export_preset.hpp`), so a user file
  passed by path behaves exactly like a built-in. An unsupported `schemaVersion`
  is rejected with a typed `IncompatibleVersion` error naming both the found and
  the supported version, and an unrecognised field is an error rather than a
  silent drop. An unknown preset name exits with the argument-error code and
  lists the built-ins, before the remesh rather than after it.

  New CLI flags: `--preset`, `--list-presets`, `--texture-size`, `--ao-samples`,
  `--cage`. The JSON report gains `preset` (name, schema version, resolved map
  set, conventions) and `outputs` (every file produced, with its written color
  space and dimensions).

  Known limitations: the maps are written as sibling files and are **not**
  referenced from the glTF material for the glb-based presets, so those bundles
  are import-ready but not auto-wired. The built-in conventions are encoded from
  each target's documented spec and pinned by unit tests, but have not been
  verified inside Blender, Unity or Unreal.

- **In-memory mesh copy and position write-back** (`cyber_mesh_clone`,
  `cyber_mesh_set_positions`; `Mesh.copy` and `Mesh.set_positions` / the
  `Mesh.positions` setter in Python). There was previously no way to duplicate
  a mesh or restore a previous state without a `save_obj` / `load_obj` round
  trip, which narrows to the OBJ text precision — `examples/16_soft_selection.py`
  was reporting ~8e-07 of spurious vertex movement from that alone, and now
  reports exactly 0. `cyber_mesh_clone` copies the whole handle (geometry,
  statistics, hidden-face and tagged-edge overlays, the soft-selection weight
  field and its saved slots) and preserves element ids, so it works as an undo
  snapshot. `cyber_mesh_set_positions` takes the same compacted order
  `cyber_mesh_copy_positions` returns and rejects a count mismatch rather than
  pairing positions with the wrong vertices.

- **Edge -> vertex-pair and vertex -> position accessors in Python**
  (`Mesh.edge_endpoints`, `Mesh.vertex_position`, over the existing
  `cyber_mesh_edge_endpoints` / `cyber_mesh_vertex_position` ABI). A committed
  seam is a set of opaque edge ids, so `SeamSet.edges()` / `SeamPath.edges()`
  could not be resolved to geometry from Python and a committed seam could not
  be drawn at all; `examples/20_seam_paths.py` now renders one.

- **In-memory sculpt handoff from Python** (`Mesh.load_handoff_buffers`, over
  the existing `cyber_handoff_open_buffers` ABI). Two handoff profiles are
  documented — a file and plain in-memory buffers — but only the file one had a
  Python surface (`Mesh.load_handoff` takes a path), so an in-process producer
  had to write a temporary PLY. The buffer profile carries the same optional
  normal / colour / `material_mix` payloads and applies the SAME version gate,
  so going through memory cannot bypass it; a payload whose length does not
  match the vertex count is rejected in Python rather than read past its end
  (the C struct's pointers carry an implied length).

- **Named export presets and bundles on the C ABI and in Python**
  (`cyber_export_preset_builtin_count` / `_builtin_name` / `_resolve` /
  `_info` / `_map` / `_map_file_name` / `_set_resolution` / `_free`,
  `cyber_default_bundle_params`, `cyber_export_bundle_write` and the
  `cyber_bundle_result_*` readers; `builtin_presets`, `ExportPreset`,
  `PresetMapEntry`, `write_bundle`, `BundleResult` and `BundleFile` in
  Python). Presets were CLI-only: the C ABI had no export entry point at all,
  so `examples/19_export_presets.py` had to shell out to the `cyberremesh`
  binary and re-read its JSON report. The example now drives the bindings
  directly — it also reads each preset's declared mesh container instead of
  carrying a hard-coded extension table, and unwraps the low-poly once so all
  four bundles bake against byte-identical UVs. Preset DATA lives in core, so
  listing, resolving and reading a preset works in **every** configuration;
  only `cyber_export_bundle_write` needs the UV-gated bundle module, and
  without it the symbol is still declared and returns a runtime error naming
  the missing module. A preset file from an unsupported schema surfaces as
  `CYBER_ERR_INCOMPATIBLE_VERSION` / `IncompatibleVersionError` naming both
  versions, and an unknown preset name as an invalid-argument error listing the
  built-ins.

### Changed

- **The vendored AutoRemesher/Geogram sources are now pinned to commit
  `b43dc827edd5d39db2f2c925e1b16d5b33ec8388`.** Both fetch sites
  (`cmake/QuadCoverSolver.cmake` and `examples/reference/build_autoremesher.sh`)
  used to `git clone --depth 1` upstream HEAD, so the quad-cover field solver —
  which every benchmark number flows through — depended on when the checkout
  happened. This was not hypothetical: a stale checkout
  (`e2d9b6f4`) produced a solver that was nondeterministic on the bench sphere
  (696/810 quads across identical runs) while the current upstream commit is
  deterministic (622 quads, 17 singularities, five identical runs). The commit
  now lives in one place, `examples/reference/autoremesher.pin`, read by both
  consumers; the sources are fetched as a shallow fetch of that exact SHA, and
  a checkout found at any other commit is loudly re-fetched to the pin instead
  of silently building the wrong solver.

- **`CyberSoftTransformReport.moved` now counts DISTINCT vertices for the
  weighted relax too, not per-iteration writes.** It already meant distinct
  vertices for `cyber_retopo_selection_transform`, but
  `cyber_retopo_selection_relax` accumulated `updates.size()` once per sweep,
  so the same field meant two different things and a multi-iteration relax
  could report more moved vertices than the mesh has: 12 iterations over 78
  selected vertices of an 861-vertex mesh reported **936**, and the same call
  now reports **78**. `resnapped` is counted the same way (distinct vertices,
  so it can no longer exceed `moved`); `max_snap_distance` is unchanged.
  Positions produced by either op are bit-identical — only the report changed.
  Callers that printed the old number as a "vertex updates" total will see a
  smaller value. The C ABI header, the Python docstring and the Swift wrapper
  now all state this, along with why `moved - resnapped` can be non-zero: with
  the default `resnap_epsilon` of 0 a vertex the re-projection did not have to
  correct at all is not counted as re-snapped.
- `cyberremesh`'s reported `elapsedSeconds` now covers export as well as the
  solve. It previously stopped at the end of the remesh, which understated a
  `--preset` run by the entire bake.
- `CyberBakeParams` gained a trailing `curvatureRange` field. The struct is
  passed by pointer and callers allocate it, so a client compiled against the
  older header must be recompiled; always initialise via
  `cyber_default_bake_params`.
- `io::ErrorCode` gained `IncompatibleVersion` for versioned data files whose
  schema the engine does not support. The C ABI maps it to `CYBER_ERR_IO` like
  the other I/O codes, so no C client behaviour changes.
- `cmake/CompilerWarnings.cmake` gained `CYBER_WARNINGS_AS_ERRORS` (default
  `ON`, so CI is unchanged) as an escape hatch for an unforeseen toolchain
  diagnostic. It is no longer needed for the known one: the GCC 12/13
  `-Wstringop-overflow` false positive from inside libstdc++ is suppressed at
  its single call site instead (see Fixed), so the default configuration builds
  on Ubuntu 24.04 with warnings still enforced everywhere else.
- `libcyber_capi` now exports only its `cyber_*` C ABI (a linker version script
  on ELF, `-exported_symbols_list` on Mach-O). The ~4000 vendored
  Geogram/stb/tinygltf/tinyobj/AutoRemesher definitions linked in from the
  static archives were globally visible and therefore open to ELF
  interposition, which matters precisely where this library is meant to run:
  inside a DCC process that carries its own copy of the same third-party code.
  No `cyber_*` entry point changed.

### Fixed

- **Baked maps came out with different pixels on arm64 — the architecture the
  product ships on.** Six of the seven bake golden checksums failed on aarch64
  while x86-64 CI stayed green, and the cause was fused-multiply-add contraction,
  not the shading code: aarch64 has FMA in its baseline ISA so GCC/Clang contract
  `a * b + c` at `-O2`, while a stock x86-64 build has no FMA instruction and
  therefore cannot. That rounds a difference of products only once, so an
  exactly-degenerate barycentric denominator or ray/triangle determinant stops
  cancelling to zero and an inside test flips — a `spot.obj` bake lost two
  covered texels outright, and every map's pixels moved. `-ffp-contract=off` is
  now applied to every translation unit in the tree
  (`cmake/FloatingPoint.cmake`), which makes bake output byte-identical across
  x86-64 and aarch64 on all six sample models and closes the same latent hole on
  x86-64, where `-march=native` reproduced the arm64 failures exactly.
  Contraction cost nothing measurable to give up — neither a seven-map bake nor a
  20k-quad remesh moved outside run-to-run noise on an FMA-capable build — and
  x86-64 output is byte-identical to before the change (120 bake dumps, 5 remesh
  runs, 8 preset/bake bundles). `tests/bake/test_bake_determinism.cpp` pins the
  rule from inside the binary so a build that loses the flag says so instead of
  leaving a checksum mismatch to be misread as a shading bug. Two narrower
  cross-architecture divergences that this does *not* close (glibc `atan2f` in UV
  chart re-orientation, and the remesh pipeline at large) are written up in
  `docs/floating-point-determinism.md`.

- **The automatic UV atlas hung on ordinary assets, uninterruptibly.** The
  default-on distortion merge (`AtlasOptions::maxChartDistortion = 0.10`) drives
  a fixpoint over adjacent chart pairs whose accept predicate LSCM-unwraps the
  *union* of the pair, and it forgot every rejection: each round re-ran the trial
  unwrap for pairs whose charts had not changed since they were last turned down,
  on charts that keep growing. Rejections are now stamped with the two charts'
  versions and skipped while those versions hold, which cuts the trial unwraps
  roughly in half and the wall clock by 1.6–4x on real models (rocker-arm 33.9s →
  8.6s, cheburashka 5.7s → 2.1s, fandisk 13.3s → 5.2s, stanford-bunny 15m20s →
  5m23s) with **byte-identical output** on all six models — the
  skipped calls are exactly the ones whose answer was already known. The pass
  remains inherently expensive at that scale, so it is now documented as such in
  `atlas.hpp`, `cyber_capi.h` and `Mesh.unwrap_atlas`, `maxChartDistortion = 0`
  is called out as the bounded path, and the whole unwrap became cancellable
  (see *Upgrade notes*).
- **Soft-selection weights were resurrected on recycled vertex ids.** Vertex
  ids come back off the mesh's free list, so the weight of a vertex removed by
  `cyber_retopo_erase` / `_delete_faces` / `_dissolve_edges` /
  `_merge_vertices` was still in the field when the next `_create_face` handed
  that id to a brand-new vertex: the new geometry was born selected and
  `cyber_retopo_selection_transform` / `_relax` silently dragged it (a quad
  authored at z = 0 landed at z = 8.6), and the saved slots carried the ghost
  through a document save/load. Every mutating op now unselects the ids it
  killed, in the live field and in every named slot
  (`cyber::retopo::dropDeadWeights`).
- **A non-finite paint dab wiped the whole selection instead of being
  ignored.** The brush's coverage test was `d >= radius`, which is false for a
  NaN distance, so a dab with a non-finite center or pressure — a viewport
  ray-miss unprojects to NaN — "covered" every vertex whatever the radius, and
  the resulting NaN was sanitized to 0 on the way into the field. The coverage
  test is now NaN-safe, such a dab is dropped (skipped per sample inside a
  stroke, so the rest of the gesture still paints), and
  `cyber_retopo_selection_paint` reports it with `CYBER_ERR_INVALID_ARG`.
- **`cyber_retopo_selection_relax` accepted a strength its sibling rejects.**
  The value went into `RelaxParams` unvalidated while `cyber_retopo_relax`
  gates it on `[0,1]`, so a NaN slider value NaN'd every selected vertex
  position and still returned `CYBER_OK` with a plausible `moved` count. It now
  fails with `CYBER_ERR_INVALID_PARAM` and leaves the mesh untouched.
- **The default configuration could not build on GCC 13 / Ubuntu 24.04.** With
  `CYBER_WARNINGS_AS_ERRORS=ON` (the default) exactly one translation unit
  failed, on a libstdc++ `-Wstringop-overflow` false positive inlined from a
  `vector::insert` range append in `reverseCuthillMcKee`. The only escape was
  disabling warning enforcement for the whole tree. Suppressed at that one
  statement instead, guarded on `__GNUC__ >= 12 && !__clang__`, so nothing else
  loses the diagnostic.
- **`push_guides` read past the end of a short guide point.** Each point's
  components were taken with nlohmann's `operator[](size_type)`, which is
  unchecked on a `const` array node, so a point with fewer than three entries
  indexed past the underlying vector — and because nothing threw, the bridge's
  `try`/`catch` could not turn it into an error reply. Point shape is now
  validated before the components are read.
- **`SeamPath::addWaypoint` mutated the path on a rejected add.** With the
  resume marker armed and no pending waypoints, re-adding the resume vertex
  seeded the marker *before* the duplicate check, so the call returned `false`
  (`cyber_seam_path_add_waypoint` -> 0) while `waypointCount()` went 0 -> 1,
  against the header's "changing nothing" contract. The repeat check now runs
  against the marker as the effective last waypoint.
- **`-DCYBER_BUILD_RETOPO=OFF` configured cleanly and then failed at the final
  link** with `cannot find -lcyber_retopo`: capi links `cyber_retopo`
  unconditionally, and CMake demotes the never-added target to a raw link flag.
  The combination is now rejected at configure time with a message naming both
  options.
- **The native seamless solve left fractional seam translations on crease-heavy
  meshes (sharp-cube residual 0.493).** The reduced MIQ elimination can make a
  seam translation or crease lattice offset DEPENDENT on a continuous free —
  canonically a cone position `x` entering a seam loop as `t = (I - R^rho)·x`
  with `|det(I - R^{±1})| = 2` — and the greedy rounding schedule only pinned
  the independent integers, so those translations stayed at whatever fraction
  the Dirichlet optimum chose and the map was not an integer grid across those
  seams. Such continuous frees now join the rounding schedule on their joint
  sub-integer lattice (step `1/lcm(|integer coeffs|)`, half-integer for the
  `±1`-cone case), pinned by the same scheduler; reductions that were already
  integer-exact produce no lattice frees, and the output is byte-identical
  there (verified: box_sharp, cylinder, cube, sphere, torus, spot native runs).
  The sharp-cube unit gate `seamlessUvResidual < 1e-3` now passes, and fandisk
  (the one corpus mesh with violations) improves at every density 400–3000:
  singularities 61/86/80/112/99 → 46/46/40/48/39, angle_dev_mean and
  edge_length_cv down across the board, feature recall 4/5 densities up,
  count-matched within 2.4%.

- **A pinned vertex was moved by a weighted transform through every binding.**
  `cyber_capi.h` documented, for that exact block, "a vertex whose weight is 0
  (and any pinned vertex) is not moved", and the engine's `transformWeighted()`
  has always accepted a `PinSet` — but `cyber_retopo_selection_transform`
  hard-coded `nullptr` for it, so the only thing that could hold a vertex still
  through the C ABI, Python or Swift was a zero weight. Pinning a vertex at
  weight 1.0 and applying a translation moved it the full distance. The fix is
  additive: `cyber_retopo_selection_transform_pinned(mesh, xf, pinned,
  pinned_count, snapper, resnap_epsilon, out_report)` threads the list through,
  and the existing entry point is now that call with an empty list — no
  signature or struct moved. Bound as
  `Mesh.transform_selection(..., pinned=[...])` and
  `transformSelection(_:snapper:resnapEpsilon:pinned:)`. A pinned vertex is now
  excluded from `report.moved` and is bit-identical after the call.

- **`CyberSoftTransformReport.moved` vs `resnapped`: the gap is now explained
  rather than hand-waved.** `examples/16_soft_selection.py` reported 193 moved
  but only 180 re-snapped with no account of the 13. Measured: all 13 carry a
  positive but *negligible* weight (<= 1.8e-33, produced by one-ring smoothing
  at the far tail of a gradient), so the weighted blend rounds to the vertex's
  own position in float — the op writes the same value back, which is what
  `moved` counts, and the Target re-projection then has nothing to correct,
  which is why `resnapped` skips it. `moved` counts WRITES, not displacements.
  The header now says so precisely, and the example measures the tail instead
  of asserting a reason. No behaviour change.

- **The ambient-occlusion bake was banded, not shaded.** Every texel fired the
  *identical* Hammersley hemisphere set, so openness could only land on the
  `k / aoSamples` lattice **and** neighbouring texels snapped to the same rung.
  With the old default of 16 rays that is 17 possible values laid down in flat
  contours: a 256x256 bake of `spot.obj` used **14 distinct levels** across its
  26 413 charted texels, with 47.7% pinned at 255 and *nothing at all* between
  240 and 254. Two fixes: the sample set now gets a per-texel Cranley-Patterson
  rotation (a deterministic hash of the texel coordinate — still no RNG, a
  re-bake is bit-identical), which converts the banding into fine dither at zero
  extra ray cost; and `aoSamples` defaults to **64** rather than 16, because no
  amount of dithering can add rungs that a 16-ray estimator does not have. On
  that same bake: distinct levels **14 -> 45**, texels at exactly 255 **47.7% ->
  31.6%**, and the fraction of adjacent texel pairs that differ (the banding
  measure) **16.0% -> 55.1%**. Mean openness is unchanged (232.5 -> 232.7 of
  255), so the estimator is still unbiased — the map is not darker, it is
  resolved. **Behaviour change:** every AO bake produces different pixels than
  before, and the default bake costs 4x the AO rays (a 3 000-quad `spot.obj`
  preset run went 8.1s -> 11.0s end to end); pass `--ao-samples 16` /
  `BakeParams::aoSamples = 16` to keep the old budget, which now at least
  dithers. The AO entry of the field-bridge pixel checksum was re-captured for
  this; the other six maps still hold their pre-bridge bits.

- **The UV atlas over-reported coverage and over-counted charts.**
  `AtlasResult::packedArea` (`packed_area` in Python, `packedArea` in the C ABI)
  is documented as "fraction of the unit square covered", but it summed the
  packed islands' BOUNDING BOXES. A chart fills only part of its box, so the
  number ran 1.6-2.3x high on curved models — a torus reported 76.6% against
  35.5% of the UV square actually painted. It is now the summed UV face area of
  the packed charts, i.e. the coverage a texture painter sees; the box figure is
  still available, honestly named, as `packedBoxArea` / `packed_box_area` (how
  tightly the box packer placed the charts). The same
  split lands on `PackResult`: `usedArea` is real coverage, `boxArea` is the box
  fraction (`packBoxes` is given boxes only, so there the two agree).
  `chartCount` also counted charts that never land — an island whose LSCM and
  planar-projection unwraps both come out degenerate covers nothing, so the
  reported count disagreed with the visible layout. Degenerate islands are now
  reported as `droppedCharts` / `dropped_charts`, and `chartCount +
  droppedCharts` is the island count. **Behaviour change:** `packed_area` drops
  (torus 0.766 -> 0.355, bumpy sphere 0.755 -> 0.323, sphere 0.552 -> 0.344,
  cube 0.667 -> 0.766 where re-oriented square charts fill their boxes), and
  `chart_count` drops on meshes carrying degenerate faces. Callers gating on the
  old number should re-baseline against the true value.

- **Seam routing ignored convex creases.** `cyber::uv::seamEdgeCost` discounted
  an edge only when its signed dihedral was `>= creaseDegrees`, i.e. valleys
  only — a ridge reports a NEGATIVE dihedral, so on convex-creased models (every
  CAD part in the corpus: fandisk carries 761 convex crease edges against 115
  concave, rocker-arm 2993 against 716) the router degenerated to a plain
  geodesic and the feature did nothing. Creases now discount whichever way they
  bend, with `convexWeight` (`convex_weight` in Python, `convexWeight` in the C
  ABI and Swift) as a separate, tunable multiplier defaulting to `0.8`;
  `creaseDegrees` is now compared against the dihedral's magnitude. Routing
  between the ends of a convex crease chain, the share of routed length that
  actually rides the crease goes 78.7% -> 97.1% on fandisk (41/57 -> 52/57
  chains followed end to end) and 60.0% -> 81.4% on rocker-arm (48/143 ->
  70/143). **Behaviour change:** a route that used to cross a ridge at full
  cost may now follow it. Valleys are unaffected (`concaveWeight` is untouched
  at 0.35 and still strictly cheaper than a ridge), and the conservative
  default means a ridge detour is only taken when it costs less than ~25% extra
  length; set `convex_weight` to 1.0 for the old behaviour, or down toward 0.45
  for aggressive ridge-following (fandisk 100%, 57/57).

- Two real `-Werror` breaks under GCC 13 that made the tree unbuildable on
  current Ubuntu: a signed/unsigned conversion in `bimdf_quantize.cpp`'s trail
  density counters and a lambda parameter shadowing an outer `b` in its T-join
  sort comparator. Neither changes behaviour.

### Hardened — four adversarial rounds, 89 defects

The authoring-track features above were built first and hardened afterwards, in
four passes that each attacked a surface the previous one had not: the
untrusted-input parsers under sanitizers, the network bridge over a real socket,
the whole suite under ASan/UBSan/TSan/LSan with the image decoders fuzzed, and
finally the GPU backends on real hardware plus the CI/release machinery. 89
defects were closed, every one with a regression test that fails without the fix.
Seven of the first round's fixes are recorded above under *Changed* and *Fixed*
because they are ordinary defects in this release's own features; everything
else is here.

The one-line summary of why it matters: before this work, four decoders — the
document container, OBJ, glTF and PLY — could be made to read outside their
buffers from a public entry point, an ordinary mesh at site coordinates could
exhaust host memory, a Latin-1 byte could terminate a host process from the
network, and the first remesh rearranged the host's signal handlers,
`std::terminate`, `std::new_handler`, `LC_NUMERIC` and the
`std::cout`/`std::cerr` buffers.

#### Behaviour changes you can notice

Read this list before upgrading; everything else in this section is a strict
improvement with no visible contract change.

- **`sharpEdgeDegrees` now actually reaches the engine from the C ABI, Python
  and Swift.** `cyber_remesh` built the default quad-cover extractor through
  `makeQuadCoverQuadrangulator(...)` without passing the parameter, so the
  extractor ran at the factory's `featureDegrees = 40` no matter what the caller
  asked for, and `cyber_capi.h`'s own documented default of 90 could never take
  effect. The CAD crease-pinning behaviour the parameter gates was therefore
  unreachable from every binding. The CLI threaded it correctly, which is why
  this went unnoticed. **Consequence:** a default remesh through the ABI or
  Python now binds features at 90°, not 40°, so crease-heavy models come out
  with different topology than the same call produced before. Pass
  `sharpEdgeDegrees = 40.0f` (`sharp_edge_degrees=40.0`) to reproduce the old
  output exactly.
- **The isotropic stage now refuses splits it cannot resolve, and runs under a
  face budget.** A radius-1 sphere at world `(5e5, 5e5, 5e5)` — a
  centimetre-unit game world, a CAD part in site coordinates, a glTF scene with
  a baked transform — grew without bound until `bad_alloc` or the OOM killer,
  taking the host with it: `SplitPass` re-read its loop bound against the edge
  array it was growing and never checked that a split made progress. At that
  magnitude the re-triangulation diagonals quantize onto the same float grid, so
  the pass keeps manufacturing fresh over-length diagonals — a limit cycle at
  0.0765 / 0.1036 / 0.125 against a 0.054 target. Three guards now bound it: a
  per-edge coordinate-resolution floor (`4 * maxAbs(coords) * FLT_EPSILON`, ~7
  orders of magnitude below any target a mesh at ordinary coordinates asks for),
  a midpoint-made-no-progress test that also rejects non-finite positions, and a
  face budget (4× the count the finest allowed target implies) as the backstop.
  **Consequence:** an input that used to die now completes; where the split rule
  genuinely cannot converge, the result is a bounded, *under-refined* mesh rather
  than an allocation failure. Meshes at ordinary coordinates are byte-identical
  — verified across six models and three densities on both the quad-cover and
  the greedy path.
- **A preset can no longer choose where the engine writes.** `namingPattern`, a
  map's `suffix`, and the preset `name` that `{preset}` expands to are validated
  where the path is *formed*, so an absolute path, a `..` component, or any
  expansion that would leave `--output`'s directory (including one arriving
  through the caller's basename) is refused with an invalid-argument error
  naming the offending field. Round 1 gated `namingPattern` only and the
  `{preset}` token walked straight through it. **Consequence:** a preset that
  deliberately wrote next to the output directory (`../maps/{basename}.png`) now
  fails instead of writing; put the maps under the output directory, or point
  `--output` at the directory you meant.
- **Typed errors where an exception used to escape.** `parsePreset` threw a raw
  `nlohmann::type_error` instead of the typed `Result` the rest of the io layer
  returns; the PNG decoder had no output cap, so 1 MB of IDAT committed 1 GiB and
  surfaced as `std::bad_alloc` rather than the typed error the API promises (the
  inflate output is now bounded by the size the header itself implies);
  `cyber_remesh_guided` let an exception unwind across the C boundary; and
  `unwrapAtlas` / `lscmUnwrap` returned success while writing non-finite UVs into
  every corner, which is now a failure. **Consequence:** a caller that wrapped
  these in `try`/`catch` and treated "no exception" as success must check the
  status code — the failures are still reported, just as `CyberStatus` /
  `io::ErrorCode` / a Python exception type instead of whatever the parser threw.
- **The document format gained an optional attribute section, and the version
  did not move.** `Document::save` dropped every UV, vertex colour and
  feature-edge tag: the round trip returned bare positions and face indices with
  `CYBER_OK` throughout, so an application-shell autosave silently discarded UV
  work (the existing test could not see it because `Document::operator==` is
  deliberately structural-only). Sections 6 and 7 now carry the attribute columns
  and feature-edge tags of the Target and the EditMesh. `kFormatVersion` stays at
  1 **on purpose** — `load` rejects a version above its own, and unknown sections
  are skipped by their length prefix, so bumping it would make older binaries
  refuse files they can in fact read. **Consequence:** documents written by this
  build are larger and are read correctly by older binaries (minus the new
  sections); documents written by older binaries load unchanged.
- **Soft-selection slots are re-keyed to the compacted vertex numbering on
  save.** Serialization writes the mesh through `toIndexed`, which drops dead
  vertices and renumbers the survivors, but the slots were written against the
  live id space — so once the id space had a hole, every saved slot landed on the
  wrong vertices on load. Silent corruption of painted work, which is worse than
  a crash. **Consequence:** weights on dead ids no longer survive a round trip,
  which is the point: they name vertices the reloaded mesh does not have.
- **A write is not reported successful until the bytes reach the file.** The PNG
  / EXR / ZIP writer and `cyber_document_save_file` returned success on a
  buffered `write` and let the destructor swallow the flush failure, so a full
  disk or an over-quota mount looked like a written file. Both now `close()`
  explicitly and report the latched failure. **Consequence:** writes that were
  silently truncated now fail loudly.
- **The shared library exports only `cyber_*`.** See the *Changed* entry above
  for the ELF/Mach-O half. Windows was the third platform and had *no* export
  control at all: MSVC exports nothing without `__declspec(dllexport)`, so
  `cyber_capi.dll` came out with an empty export table and produced no import
  library — the install step then had no `cyber_capi.lib` and a C consumer got
  unresolved externals. `WINDOWS_EXPORT_ALL_SYMBOLS` restores exactly what the
  shipped MinGW lane already relied on. **Consequence:** on ELF, a host that was
  reaching a vendored Geogram/stb/tinygltf symbol through our `.so` no longer
  can; no `cyber_*` entry point changed.
- **The library stops rearranging the host process.** The first remesh on the
  default quad method replaced the host's `SIGSEGV`/`SIGILL`/`SIGBUS`/`SIGFPE`
  handlers, reset `SIGINT`, replaced `std::terminate` and `std::new_handler`, and
  `setenv`'d `LC_NUMERIC` — a DCC that installed a crash reporter lost it on the
  first remesh. Geogram is now initialized with `GEOGRAM_NO_HANDLER` and
  everything that flag does not cover is snapshotted and restored. One level up,
  every solve repointed the host's `std::cout`/`std::cerr` stream buffers and
  swallowed its concurrent logging; it no longer does. The FP exception mask is
  deliberately *not* restored, and the reason is recorded at the call site.
  **Consequence:** the vendored solver is silent by default (`--quiet` is
  honoured; `CYBER_QC_VERBOSE` brings its progress traces back), and host logging
  written during a solve now appears where the host put it.
- **Selecting a GPU no longer serializes the CPU.** Every GPU backend overrode
  `parallelFor` to run inline, so choosing CUDA/OpenCL/Metal made all of the
  library's CPU-side parallel loops single-threaded. The worker pool moved to the
  base class. Device work is serialized on a mutex, because the typed kernels are
  now reachable from several host workers at once.
- **The BVH is cached on the device, so the caller must not mutate it in
  place.** `raycastBvh` / `closestPointsBvh` repacked and re-uploaded the whole
  BVH on *every* call, and the AO bake calls them once per texel. Residency is
  now keyed on a fingerprint. **Consequence:** hand over a rebuilt snapshot
  rather than editing the node/triangle arrays you already passed — the contract
  is stated in `cyber/accel/backend.hpp`.
- **`CYBER_BACKEND=cpu|cuda|metal|opencl`** is a new environment override for
  the process-wide default backend — the support escape hatch for a consumer who
  can change neither the host application nor the library it loads. An unset or
  unrecognised value keeps the automatic best-first choice.
- **`cyber_mesh_edge_faces` reports what it wrote, and the true valence moved to
  a new call.** It returned `edgeFaces().size()`, so a non-manifold edge — which
  the engine supports and tags — reported 3 or more while writing at most the 2
  entries its own prototype declares the out arrays to hold; a caller that used
  the return value as a loop bound, the obvious reading, walked off the end of
  its own buffers. The count returned is now the count WRITTEN, and the
  unclamped valence is available through the new **`cyber_mesh_edge_face_count`**
  (0 for a wire edge, 1 on a boundary, 2 on a manifold interior edge, 3+ on a
  non-manifold one; -1 for a NULL mesh or a dead edge). **Consequence:** a
  non-manifold test written as `cyber_mesh_edge_faces(...) > 2` stops firing —
  it can no longer return more than 2 — so move that test to
  `cyber_mesh_edge_face_count`. Manifold meshes are unaffected: 0, 1 and 2 mean
  exactly what they always did.
- **The CLI clamps `--adaptivity` and `--sharp-edge` for real.** Both ran
  out-of-range values straight into the pipeline while printing a warning that
  said they had been clamped.
- **The Python loader no longer walks up into a build tree it does not own.** It
  used to `dlopen` from any ancestor build directory; it now does so only when an
  ancestor is genuinely a checkout of this project (`CMakeLists.txt` +
  `capi/include/cyber_capi.h`) and neither it nor the build directory is
  world-writable or owned by another user. **Consequence:** an installed package
  in a venv, `/opt` or a CI scratch tree stops picking up a stray build tree; use
  `CYBER_CAPI_LIB` to point at a library outside a checkout.

#### Memory safety and untrusted input

Every entry point that reads a file supplied by someone else was unhardened. Two
were memory-unsafe with a proof-of-concept under a sanitizer, five more crashed
or hung the ingest outright.

- `ByteReader::ensure` bounds-checked with `m_pos + n > size()`, which wraps. A
  28-byte forged document declaring a section length of 2^64−24 passed the guard,
  produced a span of ~2^64 over a 28-byte allocation, and every subsequent read
  walked the heap — reachable from the public `cyber_document_load_file`. The
  check is now `n > size() - m_pos`, which cannot underflow given the class
  invariant, and `Document::load` rejects a section longer than the bytes
  remaining before narrowing it to `size_t`.
- `importObj` bounds-checked the vertex index but not `texcoord_index` or
  `normal_index`, and the vendored tinyobjloader only *warns* about a positive
  out-of-range `vt`/`vn`: `f 1/60000000/1` segfaulted.
- The glTF accessor/bufferView/buffer indices were used raw and the declared
  count was never cross-checked against the decoded buffer; three ~460-byte files
  each segfaulted.
- A PLY handoff allocated on declared element counts with no check against the
  payload; a truncated ASCII section hung forever, a short line read past the
  token vector, and a forged list-property count committed 2.1 GB from 186 bytes.
  The truncation guard was initially placed inside `if (!elem.properties.empty())`,
  so an element declaring *zero* properties skipped it and spun to its declared
  count — a 65-byte file hung `importMesh` and every handoff entry point forever,
  reproduced on the shipping CLI and killed at 15 s. A property-less element can
  carry no data in any format, so a nonzero count is now rejected in all three
  parse paths.
- The undo journal reserved on an attacker-controlled count, and `sampleBilinear`
  cast a non-finite float to `int` in a public inline header.

#### API and contract corrections

- A recycled element id inherited the dead element's attribute row, so `flipEdge`
  scrambled corner UVs instead of resetting them as its header documents; every
  `alloc*` now clears the recycled row. The same class of bug resurrected hidden
  faces and tagged edges on recycled ids, exactly as it did soft-selection
  weights.
- `cyber_conform` mutated positions without invalidating the render cache, so the
  zero-copy buffers served stale geometry.
- `CyberSeamPath` borrowed a mesh the header promised it snapshotted, and Python's
  `SeamPath` held a borrowed `const Mesh*` without keeping the `Mesh` alive — a
  use-after-free reachable from pure Python. Every other wrapper holding a
  borrowed pointer was audited for the same gap.
- `cyber_mesh_copy_positions` was the one copy accessor not visibility-filtered,
  and four more header contracts disagreed with their implementations.
- Two Python entry points set the element count from the caller's list while
  flattening it unchecked, so C read past the ctypes buffer.
- `add_subdirectory()` consumption was broken by `${CMAKE_SOURCE_DIR}`, and the
  Python binding searched for a library filename the build never produces.

#### The network bridge, the only remotely reachable surface

- Any non-UTF-8 byte in a request made the error path interpolate it into the
  reply through `e.what()`; the unguarded `dump()` then threw `type_error.316`,
  escaped the connection thread and **terminated the host process**. No attacker
  was needed — a client sending Latin-1 text did it. Replies now dump with a
  replacing handler, peer bytes are never echoed back, and the connection handler
  catches everything.
- Connection threads were never reaped (~8 MB of address space per connection)
  and a failed spawn aborted the server. `BridgeServer::stop` closed the listener
  before joining the accept thread (caught by TSan).
- The bridge **client** trusted the server the way round 2 stopped the server
  trusting the client; the 256 MiB message ceiling is now enforced in both
  directions and mirrored in Python as `cyberbridge.MAX_MESSAGE_BYTES`.

#### Compute backends

Verified on real hardware — this machine carries CUDA and OpenCL — not by
inspection: the suite was run with the GPU as the default backend (518/518) and
the rectangular parity case was proved to catch the defect by re-introducing it.

- All three GPU backends sized and uploaded the `spmvCsr` x vector by ROW count,
  but `accel::SparseMatrix` carried no column count and the seamless solver's
  `Tuv` (nUv × W) and `Tt` (W × nUv) are genuinely rectangular. Wide matrices
  indexed past the device buffer; tall ones over-read the **caller's** host
  buffer, reproduced as a segfault against a guard page. Reachable on every GPU
  preset, because those presets do not set `CYBER_WITH_QUADCOVER` and the native
  solver is therefore the only route. The column count now travels through the
  primitive.
- The reason it survived: `test_gpu_parity` compared the CPU backend against
  itself in every configuration CI builds, and its CSR generator could only emit
  square matrices. It now sweeps square, wide and tall against every available
  backend, and no longer passes silently when a compiled-in backend is absent.
- `cuda_backend.cu` checked no CUDA status anywhere, so a failed allocation or
  launch silently returned the caller's untouched buffer. The ray/triangle test
  was not watertight at shared edges; `availableBackends()` rebuilt the context
  and recompiled kernels on every call; the `CpuBackend` nesting guard was not
  armed on the serial fast path; `metal_backend.mm` was missing two includes.
- **Metal remains uncompilable here and is the one change made by inspection
  alone.**

#### Performance — output bit-identical, and verified so

- The AO bake spawned and joined ~`hardware_concurrency` threads *per texel*.
- `accel::raycast` deep-copied the whole BVH on every call, and the GPU path
  re-uploaded it; device residency measured flat in triangle count where it
  previously grew (OpenCL 1731 ms → 470 ms at 25k triangles).
- glTF import was quadratic in triangle count with `TEXCOORD_0` or `NORMAL`, and
  `addFace` was O(n²) in face arity.
- The quad-cover path retained ~1.6 kB per island per call **forever** in
  Geogram's process-global citation registry — reachable memory, so invisible to
  LeakSanitizer, and exactly the profile that kills a long-running host. A farm
  worker or DCC plugin can now remesh for hours without growing.
- Net effect on the unit suite: 11.2 s → 5.1 s with the golden tests unchanged.

#### Packaging, CI and licensing

The release machinery is what keeps all of the above from rotting, and it had
gaps of its own.

- **The release workflow published binaries without running a single test.** The
  packaging scripts never invoked ctest. A test job now gates the artifacts, and
  its Linux leg tests the configuration the Linux package actually ships.
- **The Python wheel contained no native library**, so the PyPI lane could not
  produce a working wheel. `packaging/publish/stage_native_lib.py` stages it and
  `pyproject.toml`'s `package-data` keeps it.
- **AutoRemesher, Geogram and Eigen are compiled into shipped binaries but were
  invisible to the license gate and absent from `THIRD_PARTY_NOTICES.md`** — MIT,
  BSD-3-Clause and MPL-2.0 respectively, verified against the vendored LICENSE
  files rather than from memory. `tools/license_audit.py` now covers the vendored
  trees outside `thirdparty/` and fails when anything linked into a shipped
  binary is missing from the notices file, which ships inside each artifact.
- **Nothing re-ran the suite under a sanitizer and the fuzz campaigns left
  nothing behind.** `.github/workflows/hardening.yml` is a nightly (and manually
  triggerable) ASan/UBSan lane — run in *both* quadcover configurations, since
  the sanitizer preset otherwise builds a different quadrangulator than ships —
  plus TSan and libFuzzer. The fuzz harnesses and a seed corpus are checked in
  under `tests/fuzz/`, and replaying that corpus runs as the ordinary
  `fuzz_corpus_replay` ctest case on every CI leg.
- Undeclared runtime dependencies on `libtbb`/`libgomp`/`libz`, and an AppImage
  that bundled no libraries — the same class as the 0.2.3 Windows DLL bug. No
  lane was guaranteed to compile the vendored solver, so a release could silently
  ship the wrong field engine.
- `-DCYBER_BUILD_RETOPO=OFF` is rejected at configure time instead of failing at
  the final link (see *Fixed*), and the tree builds warning-clean under `-Werror`
  on GCC 13 / Ubuntu 24.04.

#### What was verified, and how

- **Tests:** 430 → 545 cases and 132 831 → 144 863 assertions across the six
  rounds; ctest 20/20 → 26/26. Every fix carries a regression test that fails
  without it.
- **Sanitizers:** the whole suite is free of ASan, UBSan, TSan and LeakSanitizer
  reports in project code, re-established at the final tree state across nine
  full runs — ASan+UBSan+LSan with the vendored field solver both on and off,
  and TSan on the CPU and against both CUDA and OpenCL — with the leak checker
  validated against a deliberate-leak canary.
- **No change to the product:** every mesh output is byte-identical to the
  pre-hardening baseline across 120 remesh runs, 28 preset/bake bundles and 67
  direct UV-atlas dumps. The differences that do exist each trace to a fix
  listed above: neutral map padding, the CLI `--sharp-edge` / `--adaptivity`
  clamp, and the GPU BVH residency identity.
- **Fuzzing:** `src/imageio` survived 18.6M executions under ASan/UBSan with no
  crash, hang or leak; the mesh decoders have checked-in corpora and replay on
  every CI leg.
- **GPU:** the full suite with the GPU as the default backend, 518/518, on CUDA
  and OpenCL hardware.
- **Not verified:** Metal (no Apple hardware here), and the MSVC lane has build
  files but no CI job.

## 0.2.5

### Added

- **Open / non-watertight surfaces now remesh usefully by default.** An open
  island (a surface with a genuine boundary rim) previously under-traced badly at
  low target density — an open paraboloid at a ~900-quad request came out as ~92
  huge faces at ~27° median angle. The `fixHoles` cleanup that fills the
  under-traced gaps is now on by default (opt out with `CYBER_QC_NO_OPEN_CLEANUP`);
  the same request now produces ~1744 uniform quads at ~78° median, edge-length
  CV ~0.27. What had kept it opt-in was an edge-CV blowup traced to `simplifyGraph`
  over-dissolving legitimately-valence-2 isoline samples on open surfaces; the
  cleanup now skips that step on open islands. Closed meshes are byte-identical.

### Changed

- **Sharper feature fidelity on crease-heavy CAD.** The native seamless-UV path
  now preserves crease networks through the isotropic pre-remesh (they were being
  shredded from one connected network into dozens of fragments) and aligns the
  cross field to creases where the surface is genuinely curved (a planarity gate
  keeps flat panels untouched, where alignment would degrade them). Measured on
  fandisk at matched quad count, feature-following error improves 0.75 → 0.62.
  Smooth organic models are byte-identical.

## 0.2.4

### Fixed

- **The isoline extractor closed a boundary loop twice, producing non-manifold
  output.** `IsolineExtractor::fixHoles` calls `fixHoleWithQuads` once
  score-checked and once not, relying on the first pass to consume `hole` — an
  in/out parameter. The terminal 4-gon branch returned with `hole` still
  populated, so a loop that reduced to exactly four edges was closed by the first
  call and closed again, identically, by the second. Two coincident quads are
  edge-count-manifold on their own, so nothing downstream rejected them; the
  pure-quad subdivision then gave each its own face point and turned the shared
  rim into a genuine non-manifold edge. Measured on the default `quad-cover` path
  over 5 models × 5 densities (2600–4200 quads): **stanford-bunny 40 → 0
  non-manifold edges**, with the other four models unaffected (20 of 25 cells
  bit-identical). This was the cause of bunny's long-recorded non-monotone defect
  count — it was never density noise.

### Changed

- **The benchmark now matches on achieved quad count, not on the request.**
  `target_quad_count` is a request the extractors undershoot and QuadriFlow
  overshoots, so a comparison at equal *request* was scoring densities 11–16%
  apart. Since `feature_error` falls roughly as `count^-0.5`, that density gap was
  worth a large fraction of the reported Phase 3 feature-following gaps. Phase 1
  and Phase 3 now drive both arms through a bounded count-match search, and a miss
  is reported rather than silently scored. `examples/test_count_match.py` guards
  the search and is now registered with ctest (it had lived unrun).

## 0.2.3

### Fixed

- **The published Windows zip could not run on a clean machine.** The build uses
  MinGW GCC, so the exe needs `libstdc++-6` / `libgcc_s_seh-1` /
  `libwinpthread-1` beside it; without them the loader fails and it exits 127
  ("command not found") despite the exe being present. CI worked around this by
  putting `/c/mingw64/bin` on PATH for its own smoke test — a distributable
  archive has to carry the DLLs, and now does.
- **macOS package smoke never resolved the CLI.** `hdiutil` output is
  tab-separated and the volume name contains a space ("CyberRemesher 0.2.3"), so
  whitespace-splitting `awk` returned just the version string.
- `mobile-smoke` is manual-only, matching the release lane: the ios/android
  packages are gated off on tags, so on a tag it could only fail looking for
  artifacts nothing produced.

## 0.2.2

### Fixed

- **The release workflow never published anything.** Every tag from v0.1.0 on
  failed: five of six package targets could not build what they were written to
  package, and the publish job needs all of them. A tag push looked like it
  released and silently did not.
  - `ios` / `android` / `windows-installer` are gated to an explicit
    `workflow_dispatch` input — they package an iOS/Android app that does not
    exist (`apps/mobile` is a placeholder) and need a WiX toolset that is not
    installed.
  - `windows-zip` configured the `windows-cuda` preset, which requires nvcc that
    the GitHub windows runner does not have; it now uses `cpu-headless` with
    Ninja's single-config output paths.
  - `macos` copied `apps/desktop/CyberRemesher` unconditionally; that shell is a
    placeholder on every build, so it now falls back to a CLI-only bundle.
- **Every AppImage produced so far was unrunnable.** `AppRun` exec'd
  `CyberRemesher`, which the CLI-only AppDir never contains. It now falls back
  to the CLI.
- The publish step generates release notes from the commit range instead of
  publishing an empty body, and no longer fails when a gated package is absent.

## 0.2.1

### Fixed

- `test_hole_fill_policy` asserted an exact boundary-loop count, which holds on
  the in-process Geogram backend but not on the dependency-free native one, so
  CI failed on builds without `-DCYBER_WITH_QUADCOVER=ON`. It now asserts the
  policy itself (disabling the fill leaves strictly more open boundary than
  enabling it) rather than a backend-specific number. No engine change.
- clang-format violations that had been failing CI on main since the
  manual-retopology layer landed, plus four in `quadcover_extractor.cpp` from
  the M3 hole-fill work.

## 0.2.0

Adds the manual-retopology engine layer, and fixes a remeshing parameter that
never took effect on the default path.

### Added — manual retopology

- **Manual-retopology engine layer behind the C API**, the surface the
  CyberTopology iOS app is built on: 31 new entry points covering the editing
  verbs (create face, tweak, move with geodesic falloff, relax, erase, delete),
  topology operators (insert loop, dissolve edges, merge vertices, rotate edge,
  create grid, subdivide, triangulate), build tools (build face, grow boundary
  edge, distribute path, surface cut, patch clone, extend boundary grid/fan,
  draw strip), symmetry (snap plane, apply, resymmetrize), Target snapping,
  per-element annotation state, and the stroke interpreter. `cyber_capi` now
  links `cyber_retopo`.
- New retopo headers: `loops.hpp` (quad-ring and edge-loop walks),
  `boundary.hpp` (boundary-loop walks), `picking.hpp`, `paths.hpp`,
  `dissolve.hpp`, `loop_metrics.hpp`, `stroke_interpreter.hpp`.
- Portability fixes for iOS: a Metal pipeline-state typo, a `~len` integer
  promotion in the PNG writer, and compiling out the out-of-process QuadCover
  CLI path (no `std::system` on iOS).

### Fixed

- **Benchmark measured the wrong extractor.** `examples/11_benchmark.py` scored
  the retired `instant-meshes` position-field extractor in its comparison
  phases rather than the shipped `quad-cover` default. Every conclusion drawn
  from those phases was about code that is no longer the default.
- **Benchmark could hang indefinitely.** The count-match search applied an
  unbounded multiplicative correction with a 40x ceiling. On models whose quad
  count saturates it escalated to a ~120k-quad request, allocated ~4 GB and
  never returned. Now bounded by a 4x ceiling with a saturation guard;
  regression-tested in `examples/test_count_match.py`.
- **`holeFillMaxBoundary` was ignored by the default `quad-cover` method.** The
  parameter is applied as a post-pass over the quadrangulator's output, but
  quad-cover closes holes *during* extraction against its own hard-coded limit
  of 65 — the same magic constant the spec criticises AutoRemesher for — so the
  caller's policy never took effect. The run's limit is now threaded into the
  extractor itself (`extractIsolineQuads` ->
  `IsolineExtractor::setHoleFillMaxBoundary`). Loops longer than the limit are
  left open as the spec requires, and a value below 3 disables filling.

  This also gives open surfaces a supported way to keep their rim: at
  `hole_fill_max_boundary=0` a sphere with its cap removed retains its
  boundary instead of being silently closed. The default (64) is unchanged, so
  damaged input still repairs to a watertight mesh.

  Corpus quality is byte-identical (benchmark unchanged on all five models) and
  broken-input robustness stays 7/7. Regression-tested in
  `python/cyberremesh/tests/test_hole_fill_policy.py`, covering both directions
  of the parameter and the over-limit case.

### Added

- `CYBER_QC_OPEN_CLEANUP` (experimental, opt-in): runs the isoline graph
  cleanup on open surfaces, with the input rim preserved. Partial — the
  `simplifyGraph` turn-angle guard is not implemented, so it is off by default
  and the default path is byte-unchanged.
- `version_identity` test: the four Python version declarations were unlinked
  from `CMakeLists.txt`, so a release bump could miss one and ship a wheel
  disagreeing with the engine inside it.
- `hole_fill_policy` test: covers the hole-fill policy across quadrangulators,
  including that an over-limit loop stays open when filling is enabled.

### Changed — claims corrected against measurement

- The quad-cover default beats QuadriFlow on median angle **and**
  irregular-vertex count on **3 of 5** corpus models (spot, rocker-arm,
  stanford-bunny), not "across the organic corpus". The earlier framing counted
  mechanical rocker-arm as organic and omitted cheburashka, which loses both
  axes. Corrected in the spec, README, Python API docs and roadmap.
- Topological validity is now stated normatively: the default is at least as
  valid as QuadriFlow on all 5 models, and stays manifold on flat CAD input
  where QuadriFlow tears (cube: 0 defects vs 576 boundary edges at ~2100 quads).
- Feature-following is recorded as a known gap (fandisk 2.0x, cheburashka 2.0x,
  rocker-arm 1.5x) with its root cause — un-pinned integer-grid phase.
- README's stanford-bunny comparison quoted QuadriFlow at 80 deg / 6%
  irregular; measured is 82 deg / 4%.
- `15_uv_vs_xatlas` claimed xatlas packs "~7 pts" tighter; measured is +10 on
  average, +18 at worst. Now derived from the measured rows.

### Known issues

- The opt-in `instant-meshes` extractor still ignores `holeFillMaxBoundary` (it
  never fills). It is a retired alternative, not the default.
- On the dependency-free **native** seamless-UV backend (builds without
  `-DCYBER_WITH_QUADCOVER=ON`), `hole_fill_max_boundary=0` exposes small tears
  the extractor leaves and the hole fill was previously repairing: a solid
  plane with no hole comes out with 6 boundary loops at 0, and 1 at the default
  64. Disabling the fill on that backend therefore yields a torn mesh. The
  in-process Geogram backend does not have this. Fixing it means repairing the
  native extractor rather than relying on the hole fill to hide it.
- Open surfaces are still not first-class: `hole_fill_max_boundary=0` now keeps
  the rim, but the isoline graph cleanup that gives closed surfaces their quad
  quality remains opt-in on open ones (`CYBER_QC_OPEN_CLEANUP`, partial).
- Feature-following trails QuadriFlow (fandisk 2.0x, cheburashka 2.0x,
  rocker-arm 1.5x); the cause is un-pinned integer-grid phase and the fix needs
  per-feature-edge integer constraints in the parameterization.

## 0.1.0

First tagged release: quad remeshing, retopology, UV editing and baking in a
C++20 engine, with the `quad-cover` default quadrangulator, the one-call
automatic UV atlas, and the Python/C ABI bindings.
