#!/usr/bin/env python3
"""Load a model and convert the whole thing to good-quality quads.

Loads a mesh (OBJ / PLY / STL / glTF — the engine dispatches by extension; FBX
is NOT supported, convert it first) and remeshes it two ways:

  * quad-dominant — the fastest good result (a few residual triangles at
    irregular vertices), and
  * pure quads — every face a quad.

    examples/run.sh examples/08_load_model.py                       # procedural knot
    examples/run.sh examples/08_load_model.py --input model.obj     # your model
    examples/run.sh examples/08_load_model.py --input model.glb --target-quads 8000 --output quads.obj
"""

import argparse
import os
import tempfile

import common as c


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert a model to quads")
    parser.add_argument("--input", help="OBJ / PLY / STL / glTF model (default: a torus knot)")
    parser.add_argument("--target-quads", type=int, default=4000)
    parser.add_argument("--output", help="write the quad-dominant result to this OBJ")
    args = parser.parse_args()

    c.require_engine()
    tmp = tempfile.mkdtemp()
    input_path = args.input
    if input_path is None:
        input_path = os.path.join(tmp, "knot.obj")
        c.torus_knot_obj(input_path)
        print("input: procedural (3,2) torus knot (triangulated)")
    else:
        print(f"input: {input_path}")

    src = c.load_any(input_path)
    print(c.stat_line("input", _InVerts(src), src))

    params = c.RemeshParams(target_quad_count=args.target_quads, adaptivity=1.0)
    dominant, s1 = c.remesh_obj(input_path, params)
    pure, s2 = c.remesh_obj(
        input_path, c.RemeshParams(target_quad_count=args.target_quads, pure_quads=True)
    )
    print(c.stat_line("quad-dominant", s1, dominant))
    print(c.stat_line("pure quads", s2, pure))

    if args.output:
        with c.Mesh.load_obj(input_path) as m:
            result = c.remesh(m, params)
            result.save_obj(args.output)
            result.close()
        print(f"wrote {args.output}")

    qd, _, _ = c.face_counts(dominant)
    qp, _, _ = c.face_counts(pure)
    panels = [src, dominant, pure]
    titles = [f"input · {len(src['faces'])} triangles", f"quad-dominant · {qd} quads",
              c.quad_label("pure quads", pure, vs_source=src)]
    ref = c.reference_panel(input_path, qp, source=src)
    if ref:
        panels.append(ref[0])
        titles.append(ref[1])
    c.render_panels(
        panels, titles,
        os.path.join(c.OUTPUT_DIR, "08_load_model.png"),
        suptitle="Load a model → good-quality quads — CyberRemesher vs QuadriFlow",
    )


class _InVerts:  # noqa: N801 - shim so stat_line works for the raw input
    def __init__(self, mesh):
        self.vertices = len(mesh["positions"])


if __name__ == "__main__":
    main()
