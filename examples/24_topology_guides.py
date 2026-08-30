#!/usr/bin/env python3
"""Topology guides — asking for an edge loop, not just a field bias.

A flow guide has always been a *soft* request: the cross field is biased toward
your stroke, the loops nearby lean that way, and none of them is actually pinned
to it. That is the right behaviour when you are steering flow. It is the wrong
behaviour when you want an edge loop exactly HERE — around an eye, a mouth, a
shoulder, a knee — which is most of what a retopology artist draws.

A guide now carries a mode. ``orientation`` is the old behaviour, unchanged.
``topology`` says the stroke is meant to become a curve in the mesh: it is
projected onto the surface as a connected edge path and handed to the same
machinery that makes quads run along a crease.

The measurement is on the RESULT, not the request: sample the guide, and for
each sample ask whether an output edge is both NEAR it and ALIGNED with it.
Both halves matter. A vertex landing near the stroke proves nothing, and neither
does an edge crossing it at right angles — a dense mesh has those everywhere, so
proximity alone would score a mesh that ignored the stroke completely at over
80%. Requiring alignment is what makes the number mean "the loop followed my
stroke".

Run through ``examples/run.sh``::

    examples/run.sh examples/24_topology_guides.py
"""

from __future__ import annotations

import json
import math
import os
import subprocess
import sys
import tempfile

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import common as c  # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Two loops on a sphere. The equator is the easy case — a natural field
# direction already runs along it, so even a soft bias does well. The tilted
# loop is the honest one: nothing about the geometry wants a loop there.
CASES = [
    ("equator", 0.0, "aligned with a natural field direction"),
    ("tilted 35°", 35.0, "no natural alignment to ride"),
]


def find_cli() -> str:
    for root, _dirs, files in os.walk(os.path.join(_REPO, "build")):
        if "cyberremesh" in files and os.access(os.path.join(root, "cyberremesh"), os.X_OK):
            return os.path.join(root, "cyberremesh")
    sys.exit("cyberremesh CLI not found — build it first (cmake --build --preset cpu-headless)")


def loop_points(tilt_degrees, n=64):
    a = math.radians(tilt_degrees)
    pts = []
    for i in range(n):
        t = i * 2.0 * math.pi / n
        x, y, z = math.cos(t), 0.0, math.sin(t)
        pts.append([x, y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a)])
    return pts


def read_obj(path):
    verts, faces = [], []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("v "):
                p = line.split()
                verts.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                faces.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return np.asarray(verts, dtype=float), faces


def unique_edges(faces):
    seen = set()
    for f in faces:
        for i in range(len(f)):
            a, b = f[i], f[(i + 1) % len(f)]
            seen.add((min(a, b), max(a, b)))
    return np.asarray(sorted(seen), dtype=int)


def edge_distances(points, tangents, verts, edges, max_angle_degrees=45.0):
    """Per point: (nearest edge of any orientation, nearest ALIGNED edge).

    Both are needed, and only the aligned one decides coverage. Proximity alone
    would count an edge that merely crosses the guide at right angles — which a
    dense mesh has everywhere, so a mesh that ignored the stroke completely
    would still score well.
    """
    a = verts[edges[:, 0]]
    b = verts[edges[:, 1]]
    ab = b - a
    len2 = np.einsum("ij,ij->i", ab, ab)
    len2[len2 < 1e-20] = 1e-20
    unit = ab / np.sqrt(len2)[:, None]
    cos_limit = math.cos(math.radians(max_angle_degrees))
    nearest = np.empty(len(points))
    aligned = np.empty(len(points))
    for i, p in enumerate(points):
        t = np.clip(np.einsum("ij,ij->i", p - a, ab) / len2, 0.0, 1.0)
        d = np.linalg.norm(p - (a + ab * t[:, None]), axis=1)
        nearest[i] = np.min(d)
        # Undirected: an edge running the other way along the guide still runs
        # along it.
        ok = np.abs(unit @ tangents[i]) >= cos_limit
        aligned[i] = np.min(d[ok]) if np.any(ok) else np.inf
    return nearest, aligned


def run(cli, sphere, guide_pts, mode, work, quads=1200):
    sidecar = os.path.join(work, f"g_{mode}.json")
    with open(sidecar, "w", encoding="utf-8") as fh:
        json.dump({"version": 1,
                   "guides": [{"points": guide_pts, "strength": 1.0, "radius": 0.15,
                               "mode": mode, "closed": True}]}, fh)
    out = os.path.join(work, f"o_{mode}.obj")
    proc = subprocess.run(
        [cli, "--input", sphere, "--output", out, "--quad-method", "zremesher",
         "--target-quads", str(quads), "--guides", sidecar, "--quiet"],
        capture_output=True, text=True, timeout=1800,
    )
    if proc.returncode != 0 or not os.path.exists(out):
        return None
    return out


def draw(ax, verts, faces, guide_pts, title):
    c._draw(ax, {"positions": verts, "faces": faces}, title)
    for coll in ax.collections:
        coll.set_alpha(0.55)
    g = np.asarray(guide_pts + [guide_pts[0]], dtype=float) * 1.02
    ax.plot(g[:, 0], g[:, 1], g[:, 2], color="#d62728", linewidth=2.6, zorder=6)


def main() -> None:
    cli = find_cli()
    fig = plt.figure(figsize=(5.0 * 2 * len(CASES), 5.6), dpi=130)
    panel = 0
    rows = []
    with tempfile.TemporaryDirectory() as work:
        sphere = os.path.join(work, "sphere.obj")
        c.uv_sphere_obj(sphere, rings=40, segments=60, radius=1.0)

        for case, tilt, blurb in CASES:
            guide_pts = loop_points(tilt)
            dense = loop_points(tilt, 256)
            for mode in ("orientation", "topology"):
                out = run(cli, sphere, guide_pts, mode, work)
                if out is None:
                    print(f"{case} / {mode}: remesh FAILED")
                    continue
                verts, faces = read_obj(out)
                edges = unique_edges(faces)
                mean_edge = float(np.mean(np.linalg.norm(
                    verts[edges[:, 0]] - verts[edges[:, 1]], axis=1)))
                tol = 0.25 * mean_edge
                pts = np.asarray(dense, dtype=float)
                tans = np.roll(pts, -1, axis=0) - pts
                tans /= np.linalg.norm(tans, axis=1)[:, None]
                d, d_aligned = edge_distances(pts, tans, verts, edges)
                coverage = float(np.mean(d_aligned <= tol))
                rows.append((case, mode, len(faces), coverage, float(np.mean(d)), float(np.max(d))))
                ax = fig.add_subplot(1, 2 * len(CASES), panel + 1, projection="3d")
                panel += 1
                draw(ax, verts, faces, guide_pts,
                     f"{case} — {mode}\n{coverage:.0%} of the guide has an edge RUNNING ALONG it")

    print(f"{'case':<12} {'mode':<12} {'quads':>6} {'coverage':>9} {'meanDist':>9} {'maxDist':>8}")
    for case, mode, quads, coverage, mean_d, max_d in rows:
        print(f"{case:<12} {mode:<12} {quads:>6} {coverage:>8.1%} {mean_d:>9.4f} {max_d:>8.4f}")
    by = {(c_, m): cov for c_, m, _q, cov, _md, _xd in rows}
    print()
    for case, _tilt, blurb in CASES:
        o = by.get((case, "orientation"))
        t = by.get((case, "topology"))
        if o is not None and t is not None:
            print(f"{case} ({blurb}): orientation {o:.1%} -> topology {t:.1%}")

    fig.suptitle(
        "Topology guides — the red loop is what was asked for; the quads are what came back\n"
        "orientation biases the field near the stroke · topology projects it onto the surface "
        "and makes the quads run along it",
        fontsize=12, fontweight="bold", color="#12233a",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    out_png = os.path.join(c.OUTPUT_DIR, "24_topology_guides.png")
    os.makedirs(c.OUTPUT_DIR, exist_ok=True)
    fig.savefig(out_png, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_png}")


if __name__ == "__main__":
    main()
