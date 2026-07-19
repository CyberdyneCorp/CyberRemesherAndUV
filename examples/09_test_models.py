#!/usr/bin/env python3
"""Remesh real-world community test models to quads.

Downloads a curated subset of Alec Jacobson's `common-3d-test-models`
(https://github.com/alecjacobson/common-3d-test-models) on demand, then converts
each to quad-dominant and pure quads and renders the results. This exercises the
pipeline on real scanned/organic/CAD geometry (varied topology, sharp features,
higher genus) rather than only procedural shapes.

    examples/run.sh examples/09_test_models.py                      # default set
    examples/run.sh examples/09_test_models.py --models spot fandisk
    examples/run.sh examples/09_test_models.py --target-quads 6000

Models are cached under examples/models/ (git-ignored). Each model's individual
license is set by the upstream repository.
"""

import argparse
import os

import common as c

DEFAULT_MODELS = ["spot", "fandisk", "rocker-arm", "cheburashka", "stanford-bunny"]


def main() -> None:
    parser = argparse.ArgumentParser(description="Remesh real-world test models")
    parser.add_argument("--models", nargs="+", default=DEFAULT_MODELS,
                        help=f"model names (default: {DEFAULT_MODELS}); "
                             f"choices: {sorted(c.COMMON_3D_MODELS)}")
    parser.add_argument("--target-quads", type=int, default=4000)
    args = parser.parse_args()

    c.require_engine()

    gallery, gallery_titles = [], []
    print(f"remeshing {len(args.models)} model(s) to ~{args.target_quads} quads each:\n")
    for name in args.models:
        try:
            path = c.download_model(name)
        except Exception as exc:  # noqa: BLE001 - network/offline: skip, keep going
            print(f"  skipping {name}: could not download ({exc})\n")
            continue
        src = c.load_any(path)

        dominant, s_dom = c.remesh_obj(
            path, c.RemeshParams(target_quad_count=args.target_quads, adaptivity=1.0))
        pure, s_pure = c.remesh_obj(
            path, c.RemeshParams(target_quad_count=args.target_quads, pure_quads=True))

        qd, _, _ = c.face_counts(dominant)
        qp, _, _ = c.face_counts(pure)
        print(c.stat_line(f"{name} (input)", _Verts(len(src["positions"])), src))
        print(c.stat_line(f"{name} quad-dom", s_dom, dominant))
        print(c.stat_line(f"{name} pure", s_pure, pure))
        print()

        # Per-model before/after triptych.
        c.render_panels(
            [src, dominant, pure],
            [f"{name} · {len(src['faces'])} tris", f"quad-dominant · {qd} quads",
             f"pure quads · {qp} quads"],
            os.path.join(c.OUTPUT_DIR, f"09_{name}.png"),
            suptitle=f"{name}: real model → quads",
        )

        gallery.append(dominant)
        gallery_titles.append(f"{name} · {qd} quads")

    # Combined gallery of the quad-dominant results.
    if gallery:
        c.render_grid(
            gallery, gallery_titles, os.path.join(c.OUTPUT_DIR, "09_gallery.png"),
            cols=3, suptitle="Common 3D test models → quad-dominant retopology",
        )
    else:
        print("no models available (offline?) — nothing rendered")


class _Verts:  # noqa: N801 - shim so stat_line can report the raw input's vertex count
    def __init__(self, n: int):
        self.vertices = n
        self.quads = 0
        self.triangles = 0


if __name__ == "__main__":
    main()
