# CyberRemesher — Python binding examples

A gallery of runnable examples that drive the engine through the **`cyberremesh`
Python binding** (ctypes over the C ABI) and render the results to PNG with
matplotlib. Each script generates a procedural input mesh, runs the remeshing
pipeline with a specific feature highlighted, and writes an image to
`examples/output/`.

Faces are coloured by arity: **blue = quad, orange = triangle, grey = n-gon**,
so quad-dominance reads at a glance.

## What each example shows

| Script | Feature | Output |
|--------|---------|--------|
| `01_quad_remesh.py` | triangle mesh → quad-dominant remesh | `output/01_quad_remesh.png` |
| `02_target_density.py` | `target_quad_count` drives density (3 levels) | `output/02_target_density.png` |
| `03_adaptivity.py` | curvature `adaptivity` (uniform vs adaptive) | `output/03_adaptivity.png` |
| `04_sharp_edges.py` | `sharp_edge_degrees` feature preservation on a cube | `output/04_sharp_edges.png` |
| `05_pure_quads.py` | `pure_quads` eliminates residual triangles | `output/05_pure_quads.png` |
| `06_hole_fill.py` | `hole_fill_max_boundary` cleanup pass | `output/06_hole_fill.png` |
| `07_baking.py` | surface baking: normal / AO / displacement maps | `output/07_baking.png` (+ `07_bake_*.png`) |
| `08_load_model.py` | load a model → quad-dominant + 100% pure-quads | `output/08_load_model.png` |
| `09_test_models.py` | remesh real community test models (spot, fandisk, bunny…) | `output/09_gallery.png` (+ `09_<model>.png`) |
| `run_all.py` | runs all of the above + a stitched `output/gallery.png` | `output/gallery.png` |

`08_load_model.py` loads a mesh and converts it to quads. It defaults to a
procedural torus knot but takes `--input <file>` for your own model:

```sh
examples/run.sh examples/08_load_model.py --input model.obj --target-quads 8000 --output quads.obj
```

**Supported input formats: OBJ, PLY, STL, glTF/GLB** (the loader dispatches by
extension). **FBX is not supported** — convert it to one of the above first
(e.g. in Blender). Two results are produced: *quad-dominant* (a few residual
triangles at irregular vertices) and *pure quads* (100% quads via subdivision).

`09_test_models.py` runs the pipeline on real geometry from Alec Jacobson's
[`common-3d-test-models`](https://github.com/alecjacobson/common-3d-test-models)
— a curated subset (smooth `spot`, CAD `fandisk` with sharp creases, higher-genus
`rocker-arm`, organic `cheburashka`, and the scanned `stanford-bunny`). Models
are downloaded on demand into `examples/models/` (git-ignored; each model's
license is set upstream). Pick your own with `--models spot fandisk …`:

```sh
examples/run.sh examples/09_test_models.py --models spot rocker-arm --target-quads 6000
```

`07_baking.py` bakes a bumpy high-poly sphere onto a smooth low-poly's UV
layout (tangent-space normal + displacement) and the detailed surface's own
ambient occlusion, via `bake(low, high, BakeMap.NORMAL, BakeParams(...))`. Ray
casting dispatches through the accel layer, so a `linux-cuda` build runs the
bake on the GPU. Each map is also written straight to PNG by the engine
(`Image.save_png`).

## Running

The examples need the C-ABI **shared** library. Build it, then use the wrapper
(`run.sh` finds the library, sets `CYBER_CAPI_LIB` + `PYTHONPATH`, and preloads
the system `libstdc++` when the interpreter's is too old — e.g. Anaconda):

```sh
# from the repo root
cmake --preset cpu-headless
cmake --build --preset cpu-headless --target cyber_capi_shared

pip install -r examples/requirements.txt

examples/run.sh examples/run_all.py        # everything + the gallery
examples/run.sh examples/01_quad_remesh.py # a single example
```

If you have a GPU build (`--preset linux-cuda`), `run.sh` will pick up that
shared library instead and the pipeline's accelerated hot spots run on the GPU.

## How it works

`cyberremesh` exposes the remeshing pipeline: `Mesh.load_obj` / `save_obj`,
`remesh(mesh, RemeshParams(...))` with progress/cancel, and `Statistics`. The
examples remesh an OBJ, save the result, parse it back for rendering, and print
a one-line stat summary (vertex/quad/triangle counts, quad %). Everything the
engine does here is reached purely through the public Python API.
