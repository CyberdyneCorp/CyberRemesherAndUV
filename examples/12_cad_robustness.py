#!/usr/bin/env python3
"""CAD robustness: the one axis where CyberRemesher cleanly beats QuadriFlow.

QuadriFlow's smooth field-based remesher TEARS flat/degenerate CAD geometry — on a
sharp-edged cube it leaves hundreds of boundary edges (holes/cracks). CyberRemesher's
flatness routing keeps such patches MANIFOLD (zero defects) while still handling the
curved-crease CAD parts that QuadriFlow does fine.

This panel remeshes a sharp cube plus two real CAD parts (fandisk, rocker-arm) with
both engines and counts topological defects — non-manifold edges, boundary edges
(tears on a closed input), and degenerate faces — drawing every boundary/tear edge in
red so the failure is visible, not just a number.

    examples/run.sh examples/12_cad_robustness.py

If QuadriFlow cannot be built (offline / no Eigen) the reference column is omitted.
"""

import os
import tempfile

import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection

import common as c

# Sharp-edged CAD test set: a generated cube (flat faces, 90-degree creases) plus two
# community CAD parts. The cube is the flagship — a degenerate flat case for field methods.
CUBE = os.path.join(tempfile.gettempdir(), "cad_robustness_cube.obj")


def defects(mesh: "c.MeshData") -> "dict":
    v = c.mesh_validity(mesh)
    v["total"] = v["nonmanifold"] + v["boundary"] + v["degenerate"]
    return v


def boundary_edges(mesh: "c.MeshData") -> "list":
    """Edges on exactly one face — tears/holes on a closed input."""
    count: "dict" = {}
    for f in mesh["faces"]:
        n = len(f)
        for k in range(n):
            a, b = f[k], f[(k + 1) % n]
            count[(min(a, b), max(a, b))] = count.get((min(a, b), max(a, b)), 0) + 1
    return [e for e, ccount in count.items() if ccount == 1]


def draw(ax, mesh: "c.MeshData", title: str) -> None:
    pos = mesh["positions"]
    polys = [[pos[i] for i in f] for f in mesh["faces"]]
    ax.add_collection3d(Poly3DCollection(polys, facecolors="#cfe0f5", edgecolors="#33517f",
                                         linewidths=0.3, alpha=1.0))
    bnd = boundary_edges(mesh)
    if bnd:
        segs = [[pos[a], pos[b]] for (a, b) in bnd]
        ax.add_collection3d(Line3DCollection(segs, colors="#e11", linewidths=2.2))
    lo, hi = pos.min(0), pos.max(0)
    ctr = (lo + hi) / 2.0
    span = float((hi - lo).max()) * 0.55 or 1.0
    for setlim, cc in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), ctr):
        setlim(cc - span, cc + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=24, azim=-52)
    ax.set_title(title, fontsize=10)


def main() -> None:
    c.require_engine()
    print("building QuadriFlow reference (first run compiles it)...")
    qf = c.quadriflow_binary()
    print(f"  QuadriFlow: {'ready' if qf else 'UNAVAILABLE'}\n")

    c.cube_obj(CUBE, subdiv=14)
    cases = [("cube", CUBE), ("fandisk", c.download_model("fandisk")),
             ("rocker-arm", c.download_model("rocker-arm"))]

    cols = ["ours (quad-cover)"] + (["QuadriFlow"] if qf else [])
    fig = plt.figure(figsize=(4.6 * len(cols), 4.7 * len(cases)), dpi=120)
    fig.suptitle("CAD robustness — topological defects (red = boundary tears; lower = better)",
                 fontsize=14, fontweight="bold", color="#12233a", y=0.995)

    header = f"{'model':<12}{'engine':<12}{'quads':>6}{'nonman':>7}{'bnd':>5}{'degen':>6}{'defects':>8}"
    print(header)
    print("-" * len(header))
    for r, (name, path) in enumerate(cases):
        mesh, _ = c.remesh_obj(path, c.RemeshParams(target_quad_count=2500, pure_quads=True,
                                                    adaptivity=0.0, quad_method="quad-cover"))
        panels = [(mesh, "ours (quad-cover)")]
        if qf:
            q, _, _ = c.face_counts(mesh)
            panels.append((c.quadriflow_remesh(qf, path, q), "QuadriFlow"))
        for cix, (m, label) in enumerate(panels):
            d = defects(m)
            qn, _, _ = c.face_counts(m)
            print(f"{name:<12}{label:<12}{qn:>6}{d['nonmanifold']:>7}{d['boundary']:>5}"
                  f"{d['degenerate']:>6}{d['total']:>8}")
            ax = fig.add_subplot(len(cases), len(cols), r * len(cols) + cix + 1, projection="3d")
            tag = "MANIFOLD" if d["total"] == 0 else f"{d['total']} DEFECTS"
            draw(ax, m, f"{name} · {label}\n{qn} quads · {tag}")

    fig.tight_layout(rect=(0, 0, 1, 0.97))
    out = os.path.join(c.OUTPUT_DIR, "12_cad_robustness.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"\n  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
