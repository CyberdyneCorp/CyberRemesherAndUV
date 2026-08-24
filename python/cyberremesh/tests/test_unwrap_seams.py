#!/usr/bin/env python3
"""Seam-driven unwrap through the binding.

The manual UV workflow used to dead-end here: a caller could mark seams, or
route and commit a SeamPath, and then had no way to parameterize along them —
the only unwrap the bindings reached was `unwrap_atlas`, which computes its own
cuts and ignores whatever was marked. These assert the path is whole.

Self-skips (exit 77, CTest SKIP) when the shared library is not loadable.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

# Unit cube, six welded quad faces.
_CUBE = (
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
    "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 3 4 8 7\nf 2 3 7 6\nf 4 1 5 8\n"
)

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def _write_cube():
    f = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    f.write(_CUBE)
    f.close()
    return f.name


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77

    from cyberremesh import Mesh, SeamPath, SeamSet, UnwrapSeamsParams

    src = _write_cube()
    out = tempfile.NamedTemporaryFile(suffix=".obj", delete=False)
    out.close()
    try:
        # --- an empty seam set means "do not cut", never "decide for me" -----
        with Mesh.load_obj(src) as mesh:
            res = mesh.unwrap_seams(SeamSet())
            check("empty seam set unwraps as one chart", res.chart_count == 1,
                  f"charts={res.chart_count}")
            check("empty seam set reports no seams", res.seam_edges == 0,
                  f"seams={res.seam_edges}")

        # --- marked seams define the charts ----------------------------------
        # The four edges bounding the cube's top face. Cutting them frees the lid,
        # so the mesh must come back as exactly two charts with real UV area —
        # unlike an open path, which does not separate anything.
        with Mesh.load_obj(src) as mesh:
            seams = SeamSet()
            ring = []
            for a, b in [(4, 5), (5, 6), (6, 7), (7, 4)]:
                e = mesh.edge_between(a, b)
                assert e is not None, f"no edge between {a} and {b}"
                seams.mark(e)
                ring.append(e)

            res = mesh.unwrap_seams(seams)
            check("marked seams cut the mesh in two", res.chart_count == 2,
                  f"charts={res.chart_count}")
            check("the seam count is reported back", res.seam_edges == 4,
                  f"seams={res.seam_edges}")
            check("charts cover real UV area", 0.0 < res.packed_area <= 1.0,
                  f"packed_area={res.packed_area}")
            check("nothing was dropped", res.dropped_charts == 0,
                  f"dropped={res.dropped_charts}")
            check("no chart is mirrored", res.flipped_charts == 0,
                  f"flipped={res.flipped_charts}")

            mesh.save_obj(out.name)
            text = open(out.name, "r", encoding="utf-8").read()
            check("UVs actually written", "vt " in text)
            f_lines = [ln for ln in text.splitlines() if ln.startswith("f ")]
            check("faces carry vt indices", bool(f_lines) and "/" in f_lines[0])

        # --- a margin still packs a genuine cut ------------------------------
        with Mesh.load_obj(src) as mesh:
            seams = SeamSet()
            for a, b in [(4, 5), (5, 6), (6, 7), (7, 4)]:
                seams.mark(mesh.edge_between(a, b))
            res = mesh.unwrap_seams(
                seams, UnwrapSeamsParams(pack_margin=0.01, texture_size=512,
                                         reorient_charts=False)
            )
            check("explicit params accepted", res.chart_count == 2,
                  f"charts={res.chart_count}")
            check("margin leaves the charts packed", res.packed_area > 0.0,
                  f"packed_area={res.packed_area}")

        # --- the gap this exists to close: routed path -> unwrap -------------
        # The precise chart-splitting semantics are pinned in C++ (test_uv.cpp),
        # where faceEdge gives an exact vertex-pair -> edge lookup. What matters
        # here is that the binding reaches them at all: before this, a committed
        # SeamPath had no consumer on this side of the ABI.
        with Mesh.load_obj(src) as mesh:
            seams = SeamSet()
            path = SeamPath(mesh)
            path.add_waypoint(0)
            path.add_waypoint(6)
            marked = path.commit(seams)
            check("a routed path commits seams", len(marked) > 0,
                  f"marked={len(marked)}")

            res = mesh.unwrap_seams(seams)
            check("a routed path drives an unwrap end to end", res.chart_count >= 1,
                  f"charts={res.chart_count}")
            check("the seam set handed in is the one reported",
                  res.seam_edges == len(seams.edges()),
                  f"reported={res.seam_edges} set={len(seams.edges())}")
            check("charts are packed", 0.0 < res.packed_area <= 1.0,
                  f"packed_area={res.packed_area}")

            mesh.save_obj(out.name)
            text = open(out.name, "r", encoding="utf-8").read()
            check("UVs actually written", "vt " in text)
            f_lines = [ln for ln in text.splitlines() if ln.startswith("f ")]
            check("faces carry vt indices", bool(f_lines) and "/" in f_lines[0])

        # --- sewing is the inverse of the cut --------------------------------
        with Mesh.load_obj(src) as mesh:
            seams = SeamSet()
            path = SeamPath(mesh)
            path.add_waypoint(0)
            path.add_waypoint(6)
            path.commit(seams)
            committed = seams.edges()
            assert committed, "expected the routed path to mark something"

            mesh.unwrap_seams(seams)
            mesh.stitch_seams(seams, committed)
            check("sewing clears the seams it was given", len(seams.edges()) == 0,
                  f"left={len(seams.edges())}")
            check("a sewn mesh unwraps as one chart",
                  mesh.unwrap_seams(seams).chart_count == 1)

        # --- a null seam set is refused, not silently auto-seamed ------------
        with Mesh.load_obj(src) as mesh:
            raised = False
            try:
                mesh.unwrap_seams(None)
            except Exception:
                raised = True
            check("a missing seam set is an error", raised)
    finally:
        for p in (src, out.name):
            try:
                os.unlink(p)
            except OSError:
                pass

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed: {FAILURES}")
        return 1
    print("\nall seam-driven unwrap checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
