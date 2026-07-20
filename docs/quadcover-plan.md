# QuadCover seamless-UV isoline extractor — scope & plan (Task F)

> Scoping deliverable from the E/D/F parallel workflow (2026-07-20). This is the design
> and phased plan for the structural path to QuadriFlow-grade topology; it is NOT yet
> implemented. The compilable scaffold (M0) built green in the workflow worktree but is
> intentionally not committed pending a go/no-go on the multi-week investment (see the
> honest payoff caveat in §1). Regenerate the scaffold from §2-§3 if greenlit.

---

I have everything needed. Here is my complete deliverable.

---

# TASK F — Seamless-UV isoline quad extractor: SCOPE + SCAFFOLD

## ⚠️ Worktree/base note for the integrator
This worktree is checked out at commit **c6ca581**, which **predates** `quad_extract.cpp`, `position_field.hpp`, `makeIntegerQuadrangulator/makeInstantMeshesQuadrangulator`, and the CAPI `quadMethod` enum — all of which exist on `main` (0146998) and are what the task description references. I confirmed this by diffing against the main working copy. Because of that divergence I deliberately made the scaffold **self-contained**: it depends only on the **stable `IQuadrangulator` seam** in `src/core/include/cyber/core/quadrangulate.hpp` (byte-identical in both trees), so it **compiles here AND drops cleanly into `main`** with no conflicts. The two CMake edits are additive one-liners; the three new files are additive. I did **not** touch the CAPI enum in this worktree because it doesn't exist at this base — see §2 for the trivial one-line addition to make on `main`.

---

## 1. SOLVER DECISION

The load-bearing dependency is **not** the isoline extractor (which is Qt-free and Geogram-free — the `exploragram` include in `quadextractor.cpp` is vestigial/dead). It is the **seamless integer-grid UV solver** that feeds it per-corner UVs. Three options:

**(a) Vendor Geogram's `quad_cover` into our lib — NOT RECOMMENDED as first step.**
- *Licence:* Geogram is permissive (BSD-3-style / Inria) — compatible with our MIT posture. ✅
- *Size/build impact:* ❌ Heavy. The benchmark harness compiles a *curated* Geogram subset (`geogram/basic`, `mesh`, `numerics`, plus `exploragram/hexdom/quad_cover` and `mesh_frame_field`) — dozens of TUs, needs `-DGEO_STATIC_LIBS`, system **TBB + zlib + dl**, and OpenMP. Folding that into our clean CMake-preset build (which is `-Werror -Wold-style-cast -Wconversion -Wsign-conversion`) means either (i) importing Geogram as an `ExternalProject`/`FetchContent` with warnings suppressed for that target, or (ii) vendoring ~hundreds of files under `thirdparty/`. Either way it roughly doubles configure/build time and adds a hard TBB dependency to a currently self-contained core.
- *Effort:* 3–5 days just to get it linking cleanly under our presets; ongoing maintenance burden.
- *Risk:* Medium-high. Geogram's `quad_cover` asserts on non-manifold input (`parameterizer.cpp:204` documents working around a `quad_cover.cpp:204` adjacency assert) — robustness plumbing needed.

**(b) Implement MIQ / QuadCover ourselves — the true long game, NOT NOW.**
- Mixed-Integer Quadrangulation (Bommes 2009) / QuadCover (Kälberer 2007): a frame-field + greedy-rounded mixed-integer solve over a sparse linear system. This is a **multi-month research effort** (comparable to what we already sank into the integer extractor). Needs a robust sparse solver + integer rounding loop + seam/cut-graph machinery.
- *Effort:* 2–4 months. *Risk:* High (this is exactly the class of problem where our collapse extractor already plateaus). Correct **eventual** direction for an MIT-clean production path, but not the way to de-risk Task F.

**(c) Reuse the existing benchmark-harness build path — RECOMMENDED for Milestone 1.**
- We **already compile** Geogram's `quad_cover` behind a Qt-free CLI (`examples/reference/build_autoremesher.sh` → `autoremesher_cli`, driven by `autoremesher_harness.cpp`). It ingests an OBJ and emits a seamless-UV-extracted quad OBJ.
- *Plan:* Milestone 1 shells out (or links the harness's object set) to produce the seamless UV **out-of-process**, feeding our **own** ported isoline extractor. This lets us validate the extractor and measure irregular-% **before** committing to vendoring (a) or writing (b). It reuses a proven build, adds **zero** deps to our core lib, and is reversible.

**Recommendation:** **(c) now → (a) if the numbers justify it → (b) only as a long-term MIT-purity play.** Sequence so the solver is validated *before* the extractor port is finished.

**Honest caveat (roadmap §6·F, `singularity-gap` conclusion §6·H):** we already benchmarked AutoRemesher's *own* `quad_cover` + isoline pipeline. It measured **irregular 6–15%** (spot 6, fandisk 14, cheb 15, bunny 12) — **no better than our integer extractor (7–13%)** and with the **worst edge-length CV of all references (0.41–0.56)**. QuadriFlow (1–4%) remains alone at the top. So the "1–4% via seamless UV" target is **not guaranteed by the isoline paradigm itself** — the win lives in **solver quality** (frame field + seamless-UV integrity), not the tracer. This reframes the phased plan (§4): the extractor is necessary structure, but the decisive lever is a QuadriFlow-grade field/solve. I encoded this warning directly in the header so the next engineer can't miss it.

---

## 2. ARCHITECTURE

**File layout (under `src/quadrangulate/`, matching the existing `field_quadrangulator` / `position_field` pattern):**
- `include/cyber/quadrangulate/quadcover_extractor.hpp` — public seam (created).
- `src/quadcover_extractor.cpp` — impl (created; stub today).
- Future ports (Milestone 2): `src/quadcover_isoline.cpp` (the `quadextractor.cpp` port) + a small `quadcover_support.hpp` for the three helpers.

**The seam & what it needs from the field solve:**
The new `makeQuadCoverQuadrangulator()` is a drop-in `IQuadrangulator`. Its `quadrangulate(Mesh&, targetEdgeLength, …)` will:
1. `computeSeamlessUv(mesh, targetEdgeLength)` → per-corner `Vec2` UVs (`SeamlessUv`). On `main` this reuses the **already-computed orientation/position field** from `computePositionField()` (`position_field.hpp:49`) as the frame field the solver aligns to — the fields are validated; only the *seamless integer solve* is new.
2. `extractIsolineQuads(mesh, uv)` → `IsolineQuadMesh` (verts + CCW quad index lists).
3. Rewrite `mesh` in place. Then the **existing pipeline** already reprojects: `pipeline.cpp` calls `relaxQuadMesh(result.mesh, ReferenceSurface, sharpEdgeDegrees…)` after the quadrangulator, so the ported extractor inherits surface reprojection for free.

**CAPI wiring (trivial, do on `main`):** add `CYBER_QUAD_QUADCOVER` to the `CyberQuadMethod` enum and one branch in the `capi.cpp:232` dispatch lambda:
```cpp
if (quadMethod == CYBER_QUAD_QUADCOVER) return cyber::remesh::makeQuadCoverQuadrangulator();
```
(Omitted in this worktree because the enum/dispatch don't exist at base c6ca581.)

**Port scope — `quadextractor.cpp` (1096 LOC) → our Vec3/Mesh types.** Estimated **~900–1100 LOC** ported + ~150 LOC glue. Functions to port, in dependency order:
- `extractConnections` (~165 LOC) — the isoline core: per triangle, intersect each UV integer isoline with the 3 edges, then **segment each line at transversal crossings** with the perpendicular family. This is where valence-4-by-construction comes from.
- `extractEdges` / `simplifyGraph` — build the connection graph.
- `collapseTriangles`, `collapseShortEdges`, `removeSingleEndpoints`, `collapseEdge` (~200 LOC) — graph cleanup.
- `extractMesh` (~270 LOC) — orbit-walk the connection graph into oriented quads (uses per-source-triangle normals to orient rings).
- `rebuildHalfEdges`, `searchBoundaries`, `fixHoles`, `fixHoleWithQuads`, `removeIsolatedFaces`, `removeNonManifoldFaces`, `testPointInTriangle`.

**AutoRemesher helpers to port (small, self-contained):**
- **`PositionKey`** (~40 LOC) — a `Vec3` map key quantised at `m_toIntFactor = 100000` (1e-5) that welds coincident cross points across triangles. Trivial to port to a `std::map<PositionKey, size_t>`.
- **`Double`** (~10 LOC, header-only) — `isZero`/`isEqual` epsilon compares (`<= numeric_limits<double>::epsilon()`). Port as free functions.
- **`MeshSeparator`** (~60–100 LOC) — `splitToIslands` + `buildEdgeToFaceMap`, used only by `removeIsolatedFaces`/`removeNonManifoldFaces`. Can be stubbed initially (skip non-manifold cleanup) and ported last.

**Key type-mapping notes:** AutoRemesher uses `Vector3`/`Vector2` (double); our `Vec3`/`Vec2` are float. The extractor's isoline arithmetic wants **double** precision for the `(int)uv` integer tests and ratio math — port the extractor's internal math in `double`, converting to `Vec3` only at mesh-write time. Watch `-Wold-style-cast`/`-Wconversion`: the reference is full of `(int)`/`(size_t)` C-casts and needs `static_cast` throughout.

---

## 3. COMPILABLE SKELETON (committed in worktree — BUILDS GREEN)

Files created (`git diff --stat`: 5 files, +239):
- `src/quadrangulate/include/cyber/quadrangulate/quadcover_extractor.hpp` — declares `makeQuadCoverQuadrangulator(int fieldIterations = 40)` plus the two collaborator seams (`SeamlessUv` + `computeSeamlessUv`, `IsolineQuadMesh` + `extractIsolineQuads`) and heavily-documented rationale/expectations.
- `src/quadrangulate/src/quadcover_extractor.cpp` — a **guarded stub**: `quadrangulate()` honours cancel, reports progress, **leaves the mesh untouched**, and returns `{success=false, failureReason="quad-cover isoline extractor not implemented (Task F scaffold)"}`. Collaborator stubs return invalid/empty results. Has the real-pipeline call sequence in comments as the next engineer's fill-in point.
- `tests/quadrangulate/test_quadcover_extractor.cpp` — 2 regression test cases (10 assertions) locking the scaffold contract: maker returns non-null, `name()=="quad-cover"`, fails cleanly without corrupting the mesh, collaborators return empty. This is the red/green target to flip as extraction lands.
- `src/quadrangulate/CMakeLists.txt` and `tests/CMakeLists.txt` — one additive line each.

The full source of all files is in the `git diff` at the end.

---

## 4. PHASED PLAN (each milestone = demoable checkpoint + test)

**M0 — Scaffold (DONE, this task).** Green stub + contract test. *Checkpoint:* `makeQuadCoverQuadrangulator()` builds, fails safely. *Test:* `test_quadcover_extractor.cpp` (passing).

**M1 — Seamless UV via harness (option c).** Fill `computeSeamlessUv` by driving the existing `autoremesher_cli`/harness object set out-of-process (OBJ in → seamless-UV quad OBJ / per-corner UV dump out). *Checkpoint:* dump per-corner UVs for a sphere and assert seamlessness (across each interior edge the two shared-vertex UVs differ only by a grid symmetry). *Test:* `computeSeamlessUv(sphere).valid == true` and a seam-consistency assertion (integer-jump residual == 0 on a closed loop).

**M2 — Port the isoline extractor.** Port `PositionKey`/`Double`/`extractConnections`/`extractEdges`/`extractMesh` to `Vec3`/`Mesh`; wire `extractIsolineQuads`. *Checkpoint:* feed M1's UV → watertight quad mesh; open it next to the harness's own output and diff. *Tests:* on a flat UV'd grid the extractor reproduces the exact NxN quad grid (0 irregular); on a sphere the output is manifold and **quad count ≈ area/spacing²**.

**M3 — Full IQuadrangulator + CAPI.** `quadrangulate()` runs M1→M2→in-place rewrite; add `CYBER_QUAD_QUADCOVER`. *Checkpoint:* end-to-end `cyber_remesh` with the new method on the golden assets, reprojected by the existing `relaxQuadMesh`. *Test:* pipeline test — output watertight, quad-dominant, no crash; wire into `11_benchmark.py` as a measured method.

**M4 — Solver quality push to 1–4% (the real research).** Only now attack the metric. Options: tune the frame field feeding `quad_cover` (adaptive scaling already exists as `facetScaling`), or begin option (a) vendor / (b) native MIQ. *Checkpoint:* benchmark irregular-% vs QuadriFlow across spot/fandisk/cheb/bunny. *Test/gate:* the harness benchmark shows irregular-% **at or below our integer extractor's 7–13%** as the pass bar; **1–4% is the stretch goal** — and per §6·F may require a better solve than stock `quad_cover`, which is the decision point for whether to invest in (b).

---

## BUILD & TEST RESULTS

- `cmake --preset cpu-headless` → configure exit 0.
- `cmake --build --preset cpu-headless --target cyber_tests` → **exit 0** (object `quadcover_extractor.cpp.o` built; libs + test exe linked). No new warnings under `-Werror -Wconversion -Wold-style-cast`.
- `LD_PRELOAD=… ./build/cpu-headless/tests/cyber_tests`:
  - New tests only: **2 cases / 10 assertions — all passed.**
  - **Full suite: 205 cases / 118,579 assertions — all passed, 0 failed, 0 skipped.**

## GIT STATUS (new files)
```
Untracked (now intent-to-add):
  src/quadrangulate/include/cyber/quadrangulate/quadcover_extractor.hpp
  src/quadrangulate/src/quadcover_extractor.cpp
  tests/quadrangulate/test_quadcover_extractor.cpp
Modified:
  src/quadrangulate/CMakeLists.txt
  tests/CMakeLists.txt
```
