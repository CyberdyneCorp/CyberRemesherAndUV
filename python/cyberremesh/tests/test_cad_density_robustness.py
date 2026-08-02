#!/usr/bin/env python3
"""CAD density-robustness regressions (docs/ROADMAP.md, 2026-08-02 CAD sweep).

Two density-dependent failures on the generated CAD meshes, both fixed:

1. Cylinder hard failure / runtime blowup. At requests where the solve density
   crossed the input rim segmentation (~2300-3000 quads), the isotropic stage
   split the rim's feature edges into permanently-uncollapsible sub-band halves
   and shed degenerate cap slivers; their junk normals re-tagged hundreds of
   spurious feature seams, the seamless solve diverged, and the CLI exited 4
   ("remeshing produced no result"). At 4500 the unconverged split/collapse
   transient left a 2.4x over-dense work mesh and a 657s masked-CG crawl.

2. box_sharp request-dependent geometry drops. Fractional feature-seam offsets
   (a non-unit Gauss-Jordan pivot spreading fractional coefficients into the
   integer lattice) left every patch grid disagreeing along the creases; the
   cube caps traced as disconnected islands and the extractor's largest-island
   keep silently deleted them — a "flawless" zero-singularity grid missing both
   z faces (hausdorff ~0.22) that still reported success.

The guards below run the real remesh path on the same generated inputs at the
densities that failed, and assert the output is a CLOSED surface covering the
input area with the expected extents. Runnable as a plain script; exits 77
(CTest SKIP) when the library is absent.
"""

import math
import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

from cyberremesh import Mesh, RemeshParams, remesh

FAILURES: "list" = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


# --- generated CAD inputs (identical to tools/bench/corpus.py) -----------------


def cylinder(segments: int = 48, height_divs: int = 12):
    verts = []
    for h in range(height_divs + 1):
        z = h / height_divs - 0.5
        for s in range(segments):
            t = 2 * math.pi * s / segments
            verts.append((0.5 * math.cos(t), 0.5 * math.sin(t), z))
    top_center = len(verts)
    verts.append((0.0, 0.0, 0.5))
    bottom_center = len(verts)
    verts.append((0.0, 0.0, -0.5))
    faces = []
    for h in range(height_divs):
        for s in range(segments):
            a = h * segments + s
            b = h * segments + (s + 1) % segments
            c = (h + 1) * segments + (s + 1) % segments
            d = (h + 1) * segments + s
            faces.append((a, b, c))
            faces.append((a, c, d))
    top_row = height_divs * segments
    for s in range(segments):
        faces.append((top_center, top_row + s, top_row + (s + 1) % segments))
        faces.append((bottom_center, (s + 1) % segments, s))
    return verts, faces


def box(divisions: int = 12):
    verts = []
    faces = []

    def grid(origin, du, dv):
        base = len(verts)
        for i in range(divisions + 1):
            for j in range(divisions + 1):
                verts.append(tuple(origin[k] + du[k] * i / divisions + dv[k] * j / divisions
                                   for k in range(3)))
        for i in range(divisions):
            for j in range(divisions):
                a = base + i * (divisions + 1) + j
                b = a + divisions + 1
                faces.append((a, b, b + 1))
                faces.append((a, b + 1, a + 1))

    s = 0.5
    grid((-s, -s, s), (2 * s, 0, 0), (0, 2 * s, 0))     # +z
    grid((-s, s, -s), (2 * s, 0, 0), (0, -2 * s, 0))    # -z
    grid((-s, -s, -s), (2 * s, 0, 0), (0, 0, 2 * s))    # -y
    grid((s, s, -s), (-2 * s, 0, 0), (0, 0, 2 * s))     # +y
    grid((s, -s, -s), (0, 2 * s, 0), (0, 0, 2 * s))     # +x
    grid((-s, s, -s), (0, -2 * s, 0), (0, 0, 2 * s))    # -x
    return verts, faces


# --- helpers -------------------------------------------------------------------


def write_obj(path, verts, faces):
    with open(path, "w", encoding="utf-8") as fh:
        for v in verts:
            fh.write(f"v {v[0]:.9g} {v[1]:.9g} {v[2]:.9g}\n")
        for f in faces:
            fh.write("f " + " ".join(str(i + 1) for i in f) + "\n")


def read_obj(path):
    verts, faces = [], []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("v "):
                verts.append([float(t) for t in line.split()[1:4]])
            elif line.startswith("f "):
                faces.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return verts, faces


def surface_area(verts, faces):
    area = 0.0
    for f in faces:
        for k in range(1, len(f) - 1):
            a, b, c = verts[f[0]], verts[f[k]], verts[f[k + 1]]
            ab = [b[i] - a[i] for i in range(3)]
            ac = [c[i] - a[i] for i in range(3)]
            cx = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                  ab[0] * ac[1] - ab[1] * ac[0])
            area += 0.5 * math.sqrt(cx[0] ** 2 + cx[1] ** 2 + cx[2] ** 2)
    return area


def boundary_edges(faces):
    use: dict = {}
    for f in faces:
        for k in range(len(f)):
            a, b = f[k], f[(k + 1) % len(f)]
            key = (min(a, b), max(a, b))
            use[key] = use.get(key, 0) + 1
    return sum(1 for c in use.values() if c == 1)


def run(tmp, name, verts, faces, target, pure=False):
    src = os.path.join(tmp, f"{name}.obj")
    write_obj(src, verts, faces)
    out_path = os.path.join(tmp, f"{name}_{target}{'p' if pure else ''}.obj")
    with Mesh.load_obj(src) as mesh:
        params = RemeshParams(target_quad_count=target, pure_quads=pure,
                              quad_method="quad-cover")
        with remesh(mesh, params) as out:
            out.save_obj(out_path)
    return read_obj(out_path)


def main() -> int:
    try:
        with tempfile.TemporaryDirectory() as tmp:
            return _run(tmp)
    except Exception as exc:  # library missing / unloadable -> CTest SKIP
        if "LibraryNotFound" in type(exc).__name__ or "Failed to load" in str(exc):
            print(f"SKIP: {exc}")
            sys.exit(77)
        raise


def _run(tmp: str) -> int:
    cyl_v, cyl_f = cylinder()
    cyl_area = surface_area(cyl_v, cyl_f)
    # Bug 1: the density where the solve crossed the rim segmentation used to
    # exit 4; 4500 used to take 657s (the ctest timeout is the runtime guard).
    for target in (2510, 4500):
        verts, faces = run(tmp, "cylinder", cyl_v, cyl_f, target)
        check(f"cylinder@{target} produces a closed surface",
              boundary_edges(faces) == 0, f"{boundary_edges(faces)} boundary edges")
        cov = surface_area(verts, faces) / cyl_area
        check(f"cylinder@{target} covers the input area", cov > 0.9, f"coverage {cov:.2f}")

    box_v, box_f = box()
    box_area = surface_area(box_v, box_f)
    # Bug 2: these requests used to drop BOTH z faces of the cube (a "clean"
    # open tube with hausdorff ~0.22 reported as success).
    for target, pure in ((1400, False), (4500, False), (4500, True)):
        verts, faces = run(tmp, "box", box_v, box_f, target, pure)
        arm = "pure" if pure else "def"
        check(f"box@{target} {arm} produces a closed surface",
              boundary_edges(faces) == 0, f"{boundary_edges(faces)} boundary edges")
        cov = surface_area(verts, faces) / box_area
        check(f"box@{target} {arm} covers the input area", cov > 0.9, f"coverage {cov:.2f}")
        for ax, name in ((0, "x"), (1, "y"), (2, "z")):
            lo = min(v[ax] for v in verts)
            hi = max(v[ax] for v in verts)
            check(f"box@{target} {arm} spans {name} (no dropped face pair)",
                  lo < -0.49 and hi > 0.49, f"[{lo:.3f},{hi:.3f}]")

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed: {FAILURES}")
        return 1
    print("\nall CAD density-robustness checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
