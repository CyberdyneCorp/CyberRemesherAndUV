#!/usr/bin/env python3
"""Subdivision: add resolution to a quad mesh, with and without reprojection.

`Mesh.subdivide()` defaults to linear — Catmull-Clark *topology* with no
smoothing — so every quad becomes four around the face centroid and the new
vertices land on the existing flat facets. That multiplies the quad count but
adds no curvature.

Passing `project_to=` a surface pulls every vertex of the subdivided mesh onto
that surface, which is what recovers the curvature the coarse cage lost. The
two right-hand panels are the same subdivision with and without that step.
(`mode=Subdivision.CATMULL_CLARK` recovers curvature without a Target at all,
by running the smooth rules; this example is about the reprojection route.)

    examples/run.sh examples/16_subdivide.py
    examples/run.sh examples/16_subdivide.py --input model.fbx --target-quads 800
"""

import argparse
import os
import tempfile

import common as c
from cyberremesh import Mesh, RemeshParams, remesh


def _to_data(mesh: "Mesh") -> "c.MeshData":
    """Engine mesh -> parsed mesh data for rendering (via a temp OBJ)."""
    with tempfile.NamedTemporaryFile(suffix=".obj", delete=False) as tmp:
        path = tmp.name
    mesh.save(path)
    data = c.load_obj(path)
    os.unlink(path)
    return data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="mesh to load (.obj/.ply/.stl/.gltf/.glb/.fbx)")
    parser.add_argument("--target-quads", type=int, default=600)
    args = parser.parse_args()

    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = args.input
        if src_path is None:
            src_path = os.path.join(tmp, "knot.obj")
            c.torus_knot_obj(src_path)

        coarse_path = os.path.join(tmp, "coarse.obj")

        # The original surface doubles as the projection target: it holds the
        # detail the coarse quad cage is only an approximation of, so it has to
        # stay loaded while the subdivided copy is snapped back onto it.
        with Mesh.load(src_path) as target:
            with remesh(target, RemeshParams(target_quad_count=args.target_quads)) as result:
                coarse = _to_data(result)
                coarse_quads, _, _ = c.face_counts(coarse)
                result.save(coarse_path)

            # subdivide() edits in place, so the two variants each need their
            # own copy of the coarse result.
            with Mesh.load(coarse_path) as flat_mesh, Mesh.load(coarse_path) as projected_mesh:
                flat_faces = flat_mesh.subdivide()
                projected_faces = projected_mesh.subdivide(project_to=target)
                flat = _to_data(flat_mesh)
                projected = _to_data(projected_mesh)

        source = c.load_any(src_path)

        # The two subdivisions are visually near-identical on a smooth model, so
        # measure instead: RMS distance to the source surface, as a percentage of
        # its bounding-box diagonal. Reprojection is the half that improves it.
        flat_rms = c.surface_metrics(flat, source)["rms"]
        projected_rms = c.surface_metrics(projected, source)["rms"]

        print(f"source           : {len(source['faces'])} faces")
        print(f"quad remesh      : {coarse_quads} quads")
        print(f"subdivided       : {flat_faces} quads, {flat_rms:.3f}% RMS off the source")
        print(f"subdivided+reproj: {projected_faces} quads, {projected_rms:.3f}% RMS off the source")

        c.render_panels(
            [source, coarse, flat, projected],
            [
                f"source · {len(source['faces'])} faces",
                f"quad remesh · {coarse_quads} quads",
                f"subdivide · {flat_faces} quads · {flat_rms:.3f}% RMS",
                f"subdivide + reproject · {projected_faces} quads · {projected_rms:.3f}% RMS",
            ],
            os.path.join(c.OUTPUT_DIR, "16_subdivide.png"),
            suptitle="Subdivision quadruples the quads; reprojection puts them back on the surface",
        )


if __name__ == "__main__":
    main()
