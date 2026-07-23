#!/usr/bin/env python3
"""Hole filling: a punched plane with the cleanup pass off vs on."""

import os
import tempfile
from collections import Counter

import common as c


def open_edges(mesh) -> int:
    """Boundary edges (used by exactly one face)."""
    ec: Counter = Counter()
    for f in mesh["faces"]:
        for i in range(len(f)):
            a, b = f[i], f[(i + 1) % len(f)]
            ec[(min(a, b), max(a, b))] += 1
    return sum(1 for v in ec.values() if v == 1)


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "holed.obj")
        c.plane_with_hole_obj(src_path, n=14)
        src = c.load_obj(src_path)

        # hole_fill_max_boundary = 0 disables filling. A moderate limit (64)
        # fills the small interior hole but leaves the large outer perimeter
        # open — set it huge and the outer border would close too.
        #
        # The limit is enforced in two places, because the two extractors reach
        # holes differently: `field-aligned` leaves them for the pipeline's
        # post-pass (src/core/src/pipeline.cpp), while the default `quad-cover`
        # closes them during extraction and so takes the policy directly
        # (IsolineExtractor::setHoleFillMaxBoundary). Both honour it, and a loop
        # LONGER than the limit stays open either way — which is why the outer
        # border survives here even at limit=64.
        def run(method: str, limit: int):
            mesh, _ = c.remesh_obj(src_path, c.RemeshParams(
                target_quad_count=800, quad_method=method, hole_fill_max_boundary=limit))
            return mesh

        no_fill = run("quad-cover", 0)
        filled = run("quad-cover", 64)

        print(f"quad-cover, limit=0 : {open_edges(no_fill)} boundary edges "
              "(hole + outer border — hole preserved)")
        print(f"quad-cover, limit=64: {open_edges(filled)} boundary edges "
              "(outer border only — hole filled, border too long to fill)")

        panels = [src, no_fill, filled]
        titles = ["input · square hole",
                  "quad-cover · limit=0 · hole left open",
                  "quad-cover · limit=64 · hole filled"]
        # QuadriFlow has no targeted hole-fill pass — the contrast panel.
        for panel in c.reference_panels(src_path, c.face_counts(filled)[0]):
            panels.append(panel[0])
            titles.append(panel[1])
        c.render_panels(
            panels, titles,
            os.path.join(c.OUTPUT_DIR, "06_hole_fill.png"),
            suptitle="Hole filling as a cleanup pass (holeFillMaxBoundary) — vs QuadriFlow & AutoRemesher",
        )


if __name__ == "__main__":
    main()
