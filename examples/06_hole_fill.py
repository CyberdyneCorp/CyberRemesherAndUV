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
        # The parameter is applied as a post-pass over whatever the
        # quadrangulator produced (src/core/src/pipeline.cpp), so it can only
        # fill holes that survive extraction. `field-aligned` preserves them
        # and therefore honours the setting; the DEFAULT `quad-cover` closes
        # holes during extraction, so the knob has no effect there. That is a
        # known gap against the "Explicit cleanup policies" requirement —
        # pinned by python/cyberremesh/tests/test_hole_fill_policy.py. Demo the
        # parameter on the method that respects it, and show the default's
        # behaviour honestly beside it rather than mislabelling it.
        def run(method: str, limit: int):
            mesh, _ = c.remesh_obj(src_path, c.RemeshParams(
                target_quad_count=800, quad_method=method, hole_fill_max_boundary=limit))
            return mesh

        no_fill = run("field-aligned", 0)
        filled = run("field-aligned", 64)
        default_fills = run("quad-cover", 0)

        print(f"field-aligned, limit=0 : {open_edges(no_fill)} boundary edges "
              "(hole + outer border — hole preserved)")
        print(f"field-aligned, limit=64: {open_edges(filled)} boundary edges "
              "(outer border only — hole filled)")
        print(f"quad-cover,    limit=0 : {open_edges(default_fills)} boundary edges "
              "(outer border only — DEFAULT fills regardless; known gap)")

        panels = [src, no_fill, filled, default_fills]
        titles = ["input · square hole",
                  "field-aligned · limit=0 · hole left open",
                  "field-aligned · limit=64 · hole filled",
                  "quad-cover (default) · limit=0\nfills anyway — known gap"]
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
