#!/usr/bin/env python3
"""Zoomed diagnostic of the Stanford-bunny EARS: quad-cover (native) vs QuadriFlow
vs AutoRemesher. Highlights irregular (valence != 4) interior vertices in red so
the singularity structure on the thin ear tubes is visible."""
import os
import numpy as np
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import common as c

OUT = os.path.join(c.OUTPUT_DIR, "scratch_ears.png")


def valence(mesh):
    from collections import defaultdict
    val = defaultdict(int)
    bnd = defaultdict(int)
    edge = defaultdict(int)
    for f in mesh["faces"]:
        n = len(f)
        for k in range(n):
            a, b = f[k], f[(k + 1) % n]
            edge[(min(a, b), max(a, b))] += 1
            val[a] += 1
    boundary = set()
    for (a, b), cnt in edge.items():
        if cnt == 1:
            boundary.add(a); boundary.add(b)
    return val, boundary


def draw_ear(ax, mesh, title, bbox):
    pos = mesh["positions"]
    faces = mesh["faces"]
    lo, hi = bbox
    val, boundary = valence(mesh)
    polys, colors = [], []
    for f in faces:
        cen = np.mean([pos[i] for i in f], axis=0)
        if not (np.all(cen >= lo) and np.all(cen <= hi)):
            continue
        polys.append([pos[i] for i in f])
        n = len(f)
        colors.append("#5b9bd5" if n == 4 else "#ed9b40")
    coll = Poly3DCollection(polys, facecolors=colors, edgecolors="#20262e", linewidths=0.4)
    ax.add_collection3d(coll)
    # irregular interior vertices
    irr = [i for i in range(len(pos))
           if lo[0] <= pos[i][0] <= hi[0] and lo[1] <= pos[i][1] <= hi[1]
           and lo[2] <= pos[i][2] <= hi[2] and i not in boundary and val[i] != 4 and val[i] > 0]
    if irr:
        p = np.array([pos[i] for i in irr])
        ax.scatter(p[:, 0], p[:, 1], p[:, 2], c="#d11", s=18, depthshade=False, zorder=5)
    center = (lo + hi) / 2.0
    span = float((hi - lo).max()) * 0.55 or 1.0
    for setlim, cc in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), center):
        setlim(cc - span, cc + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=18, azim=-60)
    ax.set_title(f"{title}\n{len(irr)} irregular verts in ear", fontsize=10)


def main():
    c.require_engine()
    qf = c.quadriflow_binary()
    ar = c.autoremesher_binary()
    path = c.download_model("stanford-bunny")

    qc, _ = c.remesh_obj(path, c.RemeshParams(target_quad_count=2500, pure_quads=True,
                                              adaptivity=0.0, quad_method="quad-cover"))
    ref = c.quadriflow_remesh(qf, path, 2500) if qf else None
    aref = c.autoremesher_try(ar, path, 2500) if ar else None

    # Ear bounding box from the source: bunny ears are the tall +Y region.
    pos = qc["positions"]
    lo, hi = pos.min(0), pos.max(0)
    # ears occupy the upper part along the longest axis; find it
    ext = hi - lo
    print("bbox extent:", ext, "lo", lo, "hi", hi)
    # Take upper 45% of the tallest axis as the ear region
    ax_long = int(np.argmax(ext))
    cut = lo[ax_long] + 0.55 * ext[ax_long]
    elo = lo.copy(); ehi = hi.copy()
    elo[ax_long] = cut
    bbox = (elo, ehi)
    print("ear bbox axis", ax_long, "cut", cut)

    panels = [("quad-cover (native)", qc)]
    if ref is not None:
        panels.append(("QuadriFlow", ref))
    if aref is not None:
        panels.append(("AutoRemesher", aref))

    fig = plt.figure(figsize=(5.2 * len(panels), 5.4), dpi=130)
    fig.suptitle("Stanford-bunny EARS — irregular-vertex structure (red = valence != 4)",
                 fontsize=13, fontweight="bold")
    for k, (title, m) in enumerate(panels):
        ax = fig.add_subplot(1, len(panels), k + 1, projection="3d")
        draw_ear(ax, m, title, bbox)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(OUT, bbox_inches="tight", facecolor="white")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
