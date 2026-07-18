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
| `run_all.py` | runs all of the above + a stitched `output/gallery.png` | `output/gallery.png` |

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
