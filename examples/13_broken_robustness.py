#!/usr/bin/env python3
"""Robustness on BROKEN input — the meshes real pipelines actually feed a retopo tool.

Community benchmark models are pristine; production meshes (AI-generated, scanned, CAD
exports) are not. This example builds a set of deliberately-malformed inputs — holes,
non-manifold fins, degenerate faces, unwelded duplicate vertices, disconnected shells,
isolated vertices, and inconsistent face winding — remeshes each with quad-cover, and
checks the output is a clean, manifold quad mesh (0 topological defects).

CyberRemesher's front-end repairs these before the field solve: vertex welding, a
flood-fill winding-orientation pass, degenerate-face dropping, and small-patch policy.

    examples/run.sh examples/13_broken_robustness.py
"""

import math
import os
import random
import tempfile

import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection

import common as c


def write_obj(path, verts, faces):
    with open(path, "w") as f:
        for v in verts:
            f.write(f"v {v[0]} {v[1]} {v[2]}\n")
        for fc in faces:
            f.write("f " + " ".join(str(i + 1) for i in fc) + "\n")


def uv_sphere(n=20):
    verts, idx = [], {}
    for i in range(n + 1):
        for j in range(n + 1):
            th, ph = math.pi * i / n, 2 * math.pi * j / n
            verts.append((math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)))
            idx[(i, j)] = len(verts) - 1
    faces = []
    for i in range(n):
        for j in range(n):
            a, b, cc, d = idx[(i, j)], idx[(i + 1, j)], idx[(i + 1, j + 1)], idx[(i, j + 1)]
            faces += [(a, b, cc), (a, cc, d)]
    return verts, faces


def build_cases(tmp):
    p = os.path.join(tmp, "_cube.obj")
    c.cube_obj(p, subdiv=6)
    cube = c.load_any(p)
    cv, cf = list(map(tuple, cube["positions"])), list(map(list, cube["faces"]))
    cases = {}

    # Every case below — including a genuine open boundary (open_hole) — now
    # produces clean manifold quad output on the default build: the vendored
    # Geogram seamless solver fills and closes the hole. (The dependency-free
    # native fallback can still leave a few non-manifold edges at an open rim.)
    v, f = uv_sphere()  # open hole: drop the top 4 rings of faces -> a real boundary
    cases["open_hole"] = (v, list(f)[4 * 2 * 20:])

    v2, nf = list(cv), list(cf)  # non-manifold: a fin sharing a cube edge
    v2.append((cv[cf[0][0]][0] + 0.3, cv[cf[0][0]][1] + 0.3, cv[cf[0][0]][2]))
    nf.append([cf[0][0], cf[0][1], len(v2) - 1])
    cases["nonmanifold_fin"] = (v2, nf)

    v, f = uv_sphere()
    cases["degenerate_faces"] = (v, list(f) + [[k, k, k + 1] for k in range(20)])

    nv, nf = [], []  # unwelded: every face its own vertex copies
    for fc in cf:
        base = len(nv)
        nv += [cv[i] for i in fc]
        nf.append(list(range(base, base + len(fc))))
    cases["unwelded_verts"] = (nv, nf)

    v, f = uv_sphere(12)
    off = len(v)
    cases["disconnected_shells"] = (
        list(v) + [(x + 3, y, z) for (x, y, z) in v], list(f) + [[i + off for i in fc] for fc in f])

    v, f = uv_sphere()
    cases["isolated_verts"] = (list(v) + [(10 + i, 10, 10) for i in range(100)], f)

    v, f = uv_sphere()
    cases["inverted_winding"] = (v, [fc if i % 2 else (fc[0], fc[2], fc[1]) for i, fc in enumerate(f)])
    return cases


def boundary_edges(mesh):
    cnt = {}
    for f in mesh["faces"]:
        n = len(f)
        for k in range(n):
            a, b = f[k], f[(k + 1) % n]
            cnt[(min(a, b), max(a, b))] = cnt.get((min(a, b), max(a, b)), 0) + 1
    return [e for e, x in cnt.items() if x == 1]


def draw(ax, mesh, title):
    pos = mesh["positions"]
    ax.add_collection3d(Poly3DCollection([[pos[i] for i in f] for f in mesh["faces"]],
                                         facecolors="#cfe0f5", edgecolors="#33517f", linewidths=0.3))
    bnd = boundary_edges(mesh)
    if bnd:
        ax.add_collection3d(Line3DCollection([[pos[a], pos[b]] for a, b in bnd],
                                             colors="#e11", linewidths=1.6))
    lo, hi = pos.min(0), pos.max(0)
    ctr, span = (lo + hi) / 2.0, float((hi - lo).max()) * 0.55 or 1.0
    for sl, cc in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), ctr):
        sl(cc - span, cc + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=20, azim=-55)
    ax.set_title(title, fontsize=9)


def main():
    c.require_engine()
    tmp = tempfile.mkdtemp()
    cases = build_cases(tmp)

    print(f"{'broken case':<20}{'in faces':>9}{'result':<40}")
    print("-" * 69)
    outputs = []
    allclean = True
    for name, (v, f) in cases.items():
        p = os.path.join(tmp, name + ".obj")
        write_obj(p, v, f)
        try:
            mesh, _ = c.remesh_obj(p, c.RemeshParams(target_quad_count=1500, pure_quads=True,
                                                     adaptivity=0.0, quad_method="quad-cover"))
            q, _, _ = c.face_counts(mesh)
            vv = c.mesh_validity(mesh)
            d = vv["nonmanifold"] + vv["boundary"] + vv["degenerate"]
            allclean = allclean and d == 0
            print(f"{name:<20}{len(f):>9}  {'MANIFOLD' if d == 0 else str(d)+' DEFECTS':<12} {q} quads")
            outputs.append((name, mesh, q, d))
        except Exception as exc:  # noqa: BLE001
            allclean = False
            print(f"{name:<20}{len(f):>9}  FAILED: {exc}")

    print("-" * 69)
    print(f"{'ALL CLEAN' if allclean else 'SOME FAILED'}: "
          f"{sum(1 for o in outputs if o[3] == 0)}/{len(cases)} broken inputs -> manifold quad output")

    cols = 4
    rows = (len(outputs) + cols - 1) // cols
    fig = plt.figure(figsize=(4.2 * cols, 4.2 * rows), dpi=120)
    fig.suptitle("Robustness on broken input — every malformed mesh -> clean manifold quads",
                 fontsize=14, fontweight="bold", color="#12233a", y=0.99)
    for k, (name, mesh, q, d) in enumerate(outputs):
        ax = fig.add_subplot(rows, cols, k + 1, projection="3d")
        draw(ax, mesh, f"{name}\n{q} quads · {'MANIFOLD' if d == 0 else str(d)+' defects'}")
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    out = os.path.join(c.OUTPUT_DIR, "13_broken_robustness.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
