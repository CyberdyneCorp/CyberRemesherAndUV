#!/usr/bin/env python3
"""Automatic UV atlas — mesh in, packed UV atlas out.

The remesher turns a triangle soup into clean quads; this example takes the next
step and gives that mesh a UV parameterization automatically, with no hand-drawn
seams. ``Mesh.unwrap_atlas`` seams the surface into normal-coherent charts,
LSCM-unwraps each chart conformally, and shelf-packs them into the unit square —
the "Smart UV Project" workflow, in-engine.

For a spread of shapes it remeshes to quads, unwraps, and renders each result as
a pair: the 3D mesh tinted by chart next to the packed 2D atlas in the same
colours, annotated with chart count, conformal (angle) distortion and packing
efficiency.

    examples/run.sh examples/14_uv_atlas.py
"""

import os
import tempfile

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Polygon as MplPolygon
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

import common as c
from cyberremesh import AtlasParams, Mesh, RemeshParams, remesh


def load_obj_uv(path):
    """Parse an OBJ into (positions, uvs, face_v_indices, face_vt_indices)."""
    pos, uvs, faces_v, faces_vt = [], [], [], []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            p = line.split()
            if not p:
                continue
            if p[0] == "v" and len(p) >= 4:
                pos.append((float(p[1]), float(p[2]), float(p[3])))
            elif p[0] == "vt" and len(p) >= 3:
                uvs.append((float(p[1]), float(p[2])))
            elif p[0] == "f":
                vs, vts = [], []
                for tok in p[1:]:
                    a = tok.split("/")
                    vs.append(int(a[0]) - 1)
                    vts.append(int(a[1]) - 1 if len(a) > 1 and a[1] else -1)
                faces_v.append(vs)
                faces_vt.append(vts)
    return np.array(pos, dtype=float), np.array(uvs, dtype=float), faces_v, faces_vt


def uv_coverage(uvs, faces_vt):
    """Fraction of the unit UV square actually covered by geometry (sum of
    per-face UV areas). This is the real texel-efficiency measure: chart
    re-orientation shrinks each chart's slack, so more surface fits per texel.
    Bounding-box packing coverage does not capture this."""
    total = 0.0
    for f in faces_vt:
        if any(i < 0 for i in f) or len(f) < 3:
            continue
        p = uvs[f]
        x, y = p[:, 0], p[:, 1]
        total += abs(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1))) * 0.5
    return total


def chart_ids(uvs, faces_v, faces_vt, eps=1e-5):
    """Chart index per face by union-find over 3D edges. Two faces sharing an
    edge merge only when they assign the SAME UV to both endpoints (continuous
    parameterization); across a seam the UVs differ, so the charts stay apart.
    (The OBJ writer emits a distinct vt per corner, so UVs must be compared by
    value, not by vt index.)"""
    parent = list(range(len(faces_v)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    edges = {}  # (v_lo, v_hi) -> list of (face, uv_at_v_lo, uv_at_v_hi)
    for fi, (vs, vts) in enumerate(zip(faces_v, faces_vt)):
        n = len(vs)
        for k in range(n):
            a, b = vs[k], vs[(k + 1) % n]
            ua, ub = uvs[vts[k]], uvs[vts[(k + 1) % n]]
            lo, hi = (ua, ub) if a < b else (ub, ua)
            edges.setdefault((min(a, b), max(a, b)), []).append((fi, lo, hi))

    for lst in edges.values():
        for i in range(len(lst)):
            fi, loi, hii = lst[i]
            for j in range(i + 1, len(lst)):
                fj, loj, hij = lst[j]
                if (abs(loi[0] - loj[0]) < eps and abs(loi[1] - loj[1]) < eps
                        and abs(hii[0] - hij[0]) < eps and abs(hii[1] - hij[1]) < eps):
                    union(fi, fj)

    roots = [find(i) for i in range(len(faces_v))]
    relabel = {r: i for i, r in enumerate(sorted(set(roots)))}
    return [relabel[r] for r in roots], len(relabel)


def unwrap_model(build, target_quads=1400):
    """Generate a shape, remesh to quads, unwrap. Returns (parsed mesh, stats)."""
    tmp = tempfile.mkdtemp()
    src_path = os.path.join(tmp, "src.obj")
    build(src_path)
    with Mesh.load_obj(src_path) as src:
        quad = remesh(src, RemeshParams(target_quad_count=target_quads, pure_quads=True,
                                        adaptivity=0.0, quad_method="field-aligned"))
    res = quad.unwrap_atlas(AtlasParams(max_chart_angle_degrees=40.0))
    out_path = os.path.join(tmp, "atlas.obj")
    quad.save_obj(out_path)
    quad.close()
    return load_obj_uv(out_path), res


def draw_mesh_3d(ax, pos, faces_v, colors, title):
    polys = [[pos[i] for i in f] for f in faces_v]
    coll = Poly3DCollection(polys, facecolors=colors, edgecolors="#2a2f3a", linewidths=0.15)
    ax.add_collection3d(coll)
    lo, hi = pos.min(0), pos.max(0)
    ctr, span = (lo + hi) / 2.0, float((hi - lo).max()) * 0.58 or 1.0
    for sl, cc in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), ctr):
        sl(cc - span, cc + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=22, azim=-58)
    ax.set_title(title, fontsize=10, color="#12233a")


def draw_atlas_2d(ax, uvs, faces_vt, colors, title):
    for f, col in zip(faces_vt, colors):
        if any(i < 0 for i in f):
            continue
        ax.add_patch(MplPolygon([uvs[i] for i in f], closed=True, facecolor=col,
                                edgecolor="#2a2f3a", linewidth=0.15))
    ax.add_patch(MplPolygon([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True,
                            fill=False, edgecolor="#98a0ad", linewidth=1.0, linestyle="--"))
    ax.set_xlim(-0.03, 1.03)
    ax.set_ylim(-0.03, 1.03)
    ax.set_aspect("equal")
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_title(title, fontsize=10, color="#12233a")


def main():
    c.require_engine()
    models = [
        ("cube", lambda p: c.cube_obj(p, subdiv=10)),
        ("torus", lambda p: c.torus_obj(p, major=48, minor=24)),
        ("bumpy sphere", lambda p: c.bumpy_sphere_obj(p, rings=36, segments=54)),
        ("sphere", lambda p: c.sphere_obj(p, rings=28, segments=42)),
    ]

    print(f"{'model':<15}{'quads':>7}{'charts':>8}{'max ang':>9}{'rms ang':>9}"
          f"{'flip':>6}{'coverage':>10}")
    print("-" * 65)

    results = []
    cmap = plt.get_cmap("tab20")
    for name, build in models:
        try:
            (pos, uvs, faces_v, faces_vt), res = unwrap_model(build)
        except Exception as exc:  # noqa: BLE001
            print(f"{name:<15}  SKIPPED (remesh failed: {exc})")
            continue
        cids, ncharts = chart_ids(uvs, faces_v, faces_vt)
        colors = [cmap(cid % 20) for cid in cids]
        coverage = uv_coverage(uvs, faces_vt)
        results.append((name, pos, uvs, faces_v, faces_vt, colors, res, coverage))
        print(f"{name:<15}{len(faces_v):>7}{res.chart_count:>8}"
              f"{res.max_angle_distortion:>9.3f}{res.rms_angle_distortion:>9.3f}"
              f"{res.flipped_charts:>6}{coverage * 100:>9.1f}%")

    print("-" * 65)
    print("angle distortion is conformal error in [0,1); 0 = angle-preserving. "
          "coverage = fraction of the UV square filled by geometry (charts "
          "re-oriented to their min-area box).")

    rows = len(results)
    fig = plt.figure(figsize=(9.0, 4.3 * rows), dpi=120)
    fig.suptitle("Automatic UV atlas — quad mesh (left) unwrapped and packed (right)",
                 fontsize=14, fontweight="bold", color="#12233a", y=0.995)
    for r, (name, pos, uvs, faces_v, faces_vt, colors, res, coverage) in enumerate(results):
        ax3d = fig.add_subplot(rows, 2, 2 * r + 1, projection="3d")
        draw_mesh_3d(ax3d, pos, faces_v, colors,
                     f"{name} — {res.chart_count} charts")
        ax2d = fig.add_subplot(rows, 2, 2 * r + 2)
        draw_atlas_2d(ax2d, uvs, faces_vt, colors,
                      f"atlas — max dist {res.max_angle_distortion:.2f} · "
                      f"coverage {coverage * 100:.0f}%")
    fig.tight_layout(rect=(0, 0, 1, 0.985))
    out = os.path.join(c.OUTPUT_DIR, "14_uv_atlas.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
