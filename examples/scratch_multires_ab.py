#!/usr/bin/env python3
"""A/B the multiresolution cross-field (CYBER_QC_CROSSFIELD_MULTIRES) vs stock,
across the corpus: ear irregular count (bunny), overall irr%, edge CV, median, Nerr."""
import os
from collections import defaultdict
import numpy as np
import common as c

MODELS = ["spot", "fandisk", "rocker-arm", "cheburashka", "stanford-bunny"]


def ear_count(mesh):
    val, edge = defaultdict(int), defaultdict(int)
    for f in mesh["faces"]:
        n = len(f)
        for k in range(n):
            a, b = f[k], f[(k + 1) % n]
            edge[(min(a, b), max(a, b))] += 1
            val[a] += 1
    boundary = {v for (a, b), cnt in edge.items() if cnt == 1 for v in (a, b)}
    pos = mesh["positions"]
    lo, hi = pos.min(0), pos.max(0)
    ax = int(np.argmax(hi - lo))
    cut = lo[ax] + 0.55 * (hi - lo)[ax]
    return sum(1 for i in range(len(pos))
               if pos[i][ax] >= cut and i not in boundary and 0 < val[i] != 4)


def run(name, path, src, multires):
    if multires:
        os.environ["CYBER_QC_CROSSFIELD_MULTIRES"] = "1"
    else:
        os.environ.pop("CYBER_QC_CROSSFIELD_MULTIRES", None)
    mesh, _ = c.remesh_obj(path, c.RemeshParams(target_quad_count=2500, pure_quads=True,
                                                adaptivity=0.0, quad_method="quad-cover"))
    qq = c.quad_quality(mesh)
    sm = c.surface_metrics(mesh, src)
    q, _, _ = c.face_counts(mesh)
    ear = ear_count(mesh) if name == "stanford-bunny" else -1
    return q, ear, c.irregular_pct(mesh), qq["cv"], qq["median"], sm["normal_err"]


def main():
    c.require_engine()
    print(f"{'model':<15} {'mode':<9} {'quads':>6} {'ear':>4} {'irr%':>5} {'CV':>5} {'med':>4} {'Nerr':>5}")
    print("-" * 62)
    for name in MODELS:
        p = c.download_model(name); s = c.load_any(p)
        for mode, mr in [("stock", False), ("multires", True)]:
            q, ear, irr, cv, med, ne = run(name, p, s, mr)
            es = f"{ear:>4}" if ear >= 0 else "   -"
            print(f"{name:<15} {mode:<9} {q:>6} {es} {irr:>5.1f} {cv:>5.2f} {med:>4.0f} {ne:>5.1f}")
        print()


if __name__ == "__main__":
    main()
