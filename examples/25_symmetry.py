#!/usr/bin/env python3
"""Exact symmetry — mirrored TOPOLOGY, not merely mirrored shape.

For retopology the requirement is `left topology == mirrored right topology`,
not `left shape ~= right shape`. Those are very different asks, and the second
is what you get by remeshing a symmetric model and hoping: a field solve on a
symmetric surface is not a symmetric function of it, so cones land wherever
iteration order and floating-point ties put them and the two halves come back
with different edge counts. No amount of position averaging fixes that — the
connectivity is already different.

`--symmetry x` gets it by CONSTRUCTION instead: cut the input at the midplane,
solve one half, and mirror the connectivity. The second half is then exact by
definition.

This script measures the property directly on the result: every vertex must have
a mirror partner, and every face's mirror image must itself be a face.

Run through ``examples/run.sh``::

    examples/run.sh examples/25_symmetry.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from collections import Counter

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import common as c  # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS_DIR = os.path.join(_REPO, "examples", "models")

CORPUS = ["cheburashka", "spot"]
# `examples/models/` is git-ignored — 09_test_models.py downloads it on demand —
# so CI has no corpus. These stand in: the mirror operates on the SHAPE, so an
# input whose own tessellation is not symmetric is still a valid fixture.
PROCEDURAL = [
    ("sphere", lambda path: c.uv_sphere_obj(path, rings=32, segments=48, radius=1.0)),
    ("bumpy sphere", lambda path: c.bumpy_sphere_obj(path, rings=32, segments=48)),
]
TARGET = 2000


def find_cli() -> str:
    for root, _dirs, files in os.walk(os.path.join(_REPO, "build")):
        if "cyberremesh" in files and os.access(os.path.join(root, "cyberremesh"), os.X_OK):
            return os.path.join(root, "cyberremesh")
    sys.exit("cyberremesh CLI not found — build it first (cmake --build --preset cpu-headless)")


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


def symmetry_report(verts, faces, plane_x, tol):
    """(unmatched vertices, unmatched faces) under reflection about x = plane_x.

    Matching is NEAREST-WITHIN-TOLERANCE, not a quantized key lookup. Rounding
    positions to a grid and comparing keys looks equivalent and is not: two
    distinct vertices inside one cell collide onto a single entry, and a partner
    sitting just across a cell boundary is missed. Both make a perfectly
    symmetric mesh look asymmetric, which is exactly what a first version of
    this script reported.
    """
    from scipy.spatial import cKDTree

    tree = cKDTree(verts)
    mirrored_points = verts.copy()
    mirrored_points[:, 0] = 2.0 * plane_x - mirrored_points[:, 0]
    distance, partner = tree.query(mirrored_points, distance_upper_bound=tol)
    matched = distance <= tol
    unmatched_v = int((~matched).sum())

    face_sets = {tuple(sorted(f)) for f in faces}
    unmatched_f = 0
    for f in faces:
        if not all(matched[i] for i in f):
            unmatched_f += 1
            continue
        if tuple(sorted(int(partner[i]) for i in f)) not in face_sets:
            unmatched_f += 1
    return unmatched_v, unmatched_f


def mesh_defects(faces):
    cnt = Counter()
    for f in faces:
        for i in range(len(f)):
            a, b = f[i], f[(i + 1) % len(f)]
            cnt[(min(a, b), max(a, b))] += 1
    return (sum(1 for n in cnt.values() if n == 1), sum(1 for n in cnt.values() if n > 2))


def run(cli, src, out, symmetric):
    args = [cli, "--input", src, "--output", out, "--quad-method", "zremesher",
            "--target-quads", str(TARGET), "--quiet"]
    if symmetric:
        args += ["--symmetry", "x"]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=1800)
    return out if proc.returncode == 0 and os.path.exists(out) else None


def main() -> None:
    cli = find_cli()
    print(f"{'model':<14} {'mode':<10} {'faces':>6} {'unmatched v':>12} {'unmatched f':>12} "
          f"{'boundary':>9} {'nonmanif':>9}")
    with tempfile.TemporaryDirectory() as work:
        if all(os.path.exists(os.path.join(MODELS_DIR, f"{m}.obj")) for m in CORPUS):
            sources = [(m, os.path.join(MODELS_DIR, f"{m}.obj")) for m in CORPUS]
        else:
            print("corpus models absent (downloaded on demand) — using procedural stand-ins")
            sources = []
            for name, make in PROCEDURAL:
                path = os.path.join(work, name.replace(" ", "_") + ".obj")
                make(path)
                sources.append((name, path))

        fig = plt.figure(figsize=(5.0 * 2 * len(sources), 5.6), dpi=130)
        panel = 0
        for model, src in sources:
            src_verts, _ = read_obj(src)
            plane_x = 0.5 * (float(src_verts[:, 0].min()) + float(src_verts[:, 0].max()))
            for symmetric in (False, True):
                label = "symmetry x" if symmetric else "plain"
                out = run(cli, src, os.path.join(work, f"{model}_{int(symmetric)}.obj"), symmetric)
                if out is None:
                    print(f"{model:<14} {label:<10} remesh FAILED")
                    continue
                verts, faces = read_obj(out)
                # Tolerance from the achieved edge length: a matching radius,
                # not a grid — two distinct vertices must not collide into one.
                edges = {(min(f[i], f[(i + 1) % len(f)]), max(f[i], f[(i + 1) % len(f)]))
                         for f in faces for i in range(len(f))}
                mean_edge = float(np.mean([np.linalg.norm(verts[a] - verts[b]) for a, b in edges]))
                uv, uf = symmetry_report(verts, faces, plane_x, 0.25 * mean_edge)
                bnd, nm = mesh_defects(faces)
                print(f"{model:<14} {label:<10} {len(faces):>6} {uv:>12} {uf:>12} "
                      f"{bnd:>9} {nm:>9}")
                ax = fig.add_subplot(1, 2 * len(sources), panel + 1, projection="3d")
                panel += 1
                c._draw(ax, {"positions": verts, "faces": faces},
                        f"{model} — {label}\n"
                        f"{'EXACTLY symmetric' if uv == 0 and uf == 0 else f'{uv} v / {uf} f unmatched'}")

    fig.suptitle(
        "Exact symmetry — solve one half, mirror the CONNECTIVITY\n"
        "unmatched = vertices with no mirror partner, and faces whose mirror image is not a face",
        fontsize=12, fontweight="bold", color="#12233a",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    out_png = os.path.join(c.OUTPUT_DIR, "25_symmetry.png")
    os.makedirs(c.OUTPUT_DIR, exist_ok=True)
    fig.savefig(out_png, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_png}")


if __name__ == "__main__":
    main()
