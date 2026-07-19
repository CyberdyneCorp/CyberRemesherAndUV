#!/usr/bin/env python3
"""Phase-1 retopology benchmark: score CyberRemesher vs QuadriFlow on the quality
that actually matters — how faithfully the quads follow the source surface, how
clean the singularity structure is, and the usual angle/uniformity — across the
community test-model corpus at matched polygon count.

It also runs the Phase-2 adaptivity comparison: ours with curvature-adaptive
sizing vs QuadriFlow's uniform sizing at matched quad count. Adaptive sizing
spends polygons where the surface bends, so it should reproduce the surface more
accurately per polygon — something QuadriFlow's uniform grid cannot do.

    examples/run.sh examples/11_benchmark.py
    examples/run.sh examples/11_benchmark.py --models spot fandisk --target-quads 3000

Writes a scored summary chart to output/11_benchmark.png. If QuadriFlow cannot be
built (offline / no Eigen), the reference columns are omitted and ours still run.
"""

import argparse
import os

import matplotlib.pyplot as plt
import numpy as np

import common as c

DEFAULT_MODELS = ["spot", "fandisk", "rocker-arm", "cheburashka", "stanford-bunny"]

# Metrics and whether lower is better. These are the benchmark's North Star.
METRICS = [
    ("median", "median angle°", False),   # higher better
    ("rms", "surface dev %", True),        # lower better
    ("normal_err", "normal err°", True),   # lower better
    ("irregular", "irregular %", True),    # lower better
    ("cv", "edge CV", True),               # lower better
]


def evaluate(mesh: "c.MeshData", source: "c.MeshData") -> dict:
    """All benchmark metrics for one output mesh against its source."""
    q, _, _ = c.face_counts(mesh)
    qq = c.quad_quality(mesh)
    sm = c.surface_metrics(mesh, source)
    return {
        "quads": q,
        "median": qq["median"],
        "cv": qq["cv"],
        "slivers": qq["slivers"],
        "rms": sm["rms"],
        "normal_err": sm["normal_err"],
        "irregular": c.irregular_pct(mesh),
    }


def ours(path: str, target: int, method: str, adaptivity: float) -> "c.MeshData | None":
    try:
        mesh, _ = c.remesh_obj(
            path, c.RemeshParams(target_quad_count=target, pure_quads=True,
                                 adaptivity=adaptivity, quad_method=method))
        return mesh
    except Exception:  # noqa: BLE001
        return None


def main() -> None:
    parser = argparse.ArgumentParser(description="CyberRemesher vs QuadriFlow benchmark")
    parser.add_argument("--models", nargs="+", default=DEFAULT_MODELS,
                        help=f"choices: {sorted(c.COMMON_3D_MODELS)}")
    parser.add_argument("--target-quads", type=int, default=3000)
    args = parser.parse_args()

    c.require_engine()
    print("building QuadriFlow reference (first run compiles it)...")
    qf = c.quadriflow_binary()
    print(f"  reference: {'ready' if qf else 'UNAVAILABLE — scoring ours only'}\n")

    header = f"{'model':<15} {'engine':<20} {'quads':>6} {'med°':>5} {'dev%':>6} {'Nerr°':>6} {'irr%':>6} {'CV':>5}"
    print(header)
    print("-" * len(header))

    rows: list = []          # (model, engine, metrics) for uniform comparison
    adaptive_rows: list = []  # (model, our_adaptive_metrics, qf_metrics) for Phase 2
    wins = {m[0]: [0, 0] for m in METRICS}  # metric -> [ours_better, total] vs QF

    for name in args.models:
        try:
            path = c.download_model(name)
            src = c.load_any(path)
        except Exception as exc:  # noqa: BLE001
            print(f"{name:<15} SKIP ({exc})")
            continue

        # --- Phase 1: uniform, matched count — apples-to-apples on every metric.
        engines = {}
        fa = ours(path, args.target_quads, "field-aligned", 0.0)
        im = ours(path, args.target_quads, "instant-meshes", 0.0)
        if fa:
            engines["ours field-aligned"] = evaluate(fa, src)
        if im:
            engines["ours position-field"] = evaluate(im, src)
        ref = c.quadriflow_try(qf, path, args.target_quads)
        if ref:
            engines["QuadriFlow"] = evaluate(ref, src)

        for engine, mtr in engines.items():
            print(f"{name:<15} {engine:<20} {mtr['quads']:>6} {mtr['median']:>5.0f} "
                  f"{mtr['rms']:>6.2f} {mtr['normal_err']:>6.1f} {mtr['irregular']:>6.0f} {mtr['cv']:>5.2f}")
            rows.append((name, engine, mtr))

        # Best-of-ours vs QuadriFlow, per metric.
        if ref and (fa or im):
            for key, _, lower in METRICS:
                ours_vals = [engines[e][key] for e in engines if e.startswith("ours")]
                best = min(ours_vals) if lower else max(ours_vals)
                better = best < engines["QuadriFlow"][key] if lower else best > engines["QuadriFlow"][key]
                wins[key][0] += int(better)
                wins[key][1] += 1

        # --- Phase 2: ours adaptive vs QuadriFlow uniform at the SAME output
        # quad count (fair quality-per-polygon: run QuadriFlow at exactly the
        # count our adaptive result produced, so lower deviation means better
        # fidelity per polygon, not just more polygons).
        ad = ours(path, args.target_quads, "instant-meshes", 1.0)
        if ad:
            am = evaluate(ad, src)
            qf_matched = c.quadriflow_try(qf, path, am["quads"])
            if qf_matched:
                adaptive_rows.append((name, am, evaluate(qf_matched, src)))
        print()

    _print_verdict(wins, adaptive_rows)
    _render(rows, adaptive_rows, args.target_quads)


def _print_verdict(wins: dict, adaptive_rows: list) -> None:
    print("=" * 60)
    print("PHASE 1 — ours (best quadrangulator) vs QuadriFlow, per metric:")
    for key, label, _ in METRICS:
        won, total = wins[key]
        if total:
            print(f"  {label:<16} ours better on {won}/{total} models")
    if adaptive_rows:
        print("\nPHASE 2 — adaptivity, quality-per-polygon (ours adaptive vs QuadriFlow at")
        print("  the SAME output quad count) — lower surface dev = better fidelity per polygon:")
        better = 0
        for name, ad, ref in adaptive_rows:
            mark = "WIN " if ad["rms"] < ref["rms"] else "----"
            better += ad["rms"] < ref["rms"]
            print(f"  {mark} {name:<15} ours dev={ad['rms']:.2f}% ({ad['quads']}q)  vs  "
                  f"QuadriFlow dev={ref['rms']:.2f}% ({ref['quads']}q)")
        print(f"  adaptive better fidelity-per-polygon: {better}/{len(adaptive_rows)} models")
    print("=" * 60)


def _render(rows: list, adaptive_rows: list, target: int) -> None:
    if not rows:
        print("no results to render")
        return
    models = sorted({r[0] for r in rows}, key=lambda m: m)
    engines = ["ours field-aligned", "ours position-field", "QuadriFlow"]
    colors = {"ours field-aligned": "#5b9bd5", "ours position-field": "#2e8b57",
              "QuadriFlow": "#c05640"}
    panel_metrics = [("median", "median angle° (higher better)", False),
                     ("rms", "surface dev % (lower better)", True),
                     ("irregular", "irregular vertices % (lower better)", True)]

    fig, axes = plt.subplots(1, len(panel_metrics) + (1 if adaptive_rows else 0),
                             figsize=(5.2 * (len(panel_metrics) + (1 if adaptive_rows else 0)), 4.6),
                             dpi=130)
    fig.suptitle(f"CyberRemesher vs QuadriFlow — retopology benchmark "
                 f"(~{target} quads, uniform)", fontsize=14, fontweight="bold", color="#12233a")
    x = np.arange(len(models))
    width = 0.26
    for ax, (key, label, _lower) in zip(axes, panel_metrics):
        for e_i, engine in enumerate(engines):
            vals = []
            for m in models:
                hit = [r[2][key] for r in rows if r[0] == m and r[1] == engine]
                vals.append(hit[0] if hit else 0.0)
            ax.bar(x + (e_i - 1) * width, vals, width, label=engine, color=colors[engine])
        ax.set_xticks(x)
        ax.set_xticklabels(models, rotation=30, ha="right", fontsize=9)
        ax.set_title(label, fontsize=11)
        ax.grid(axis="y", alpha=0.25)
    axes[0].legend(fontsize=8, loc="lower right")

    if adaptive_rows:
        ax = axes[len(panel_metrics)]
        am = [r[0] for r in adaptive_rows]
        xa = np.arange(len(am))
        ax.bar(xa - width / 2, [r[1]["rms"] for r in adaptive_rows], width,
               label="ours adaptive", color="#2e8b57")
        ax.bar(xa + width / 2, [r[2]["rms"] for r in adaptive_rows], width,
               label="QuadriFlow uniform", color="#c05640")
        ax.set_xticks(xa)
        ax.set_xticklabels(am, rotation=30, ha="right", fontsize=9)
        ax.set_title("surface dev % — adaptive vs uniform\n(lower better)", fontsize=11)
        ax.grid(axis="y", alpha=0.25)
        ax.legend(fontsize=8)

    fig.tight_layout(rect=(0, 0, 1, 0.94))
    out = os.path.join(c.OUTPUT_DIR, "11_benchmark.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
