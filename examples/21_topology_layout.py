#!/usr/bin/env python3
"""Topology layout — the explicit edge-loop scaffolding behind a quad remesh.

The quad topology a remesher produces is normally an *emergent* consequence of
the cross field, the seamless grid and the isoline extraction: there is nothing
to point at and say "these are the loops, this is where the extraordinary
vertices went". The ZRemesher work makes that structure a first-class artifact —
a ``TopologyLayout`` of nodes (singularities, feature/boundary corners,
T-junctions), arcs (the separatrix curves between them) and patches (the regions
those arcs bound).

This example runs the remesher with layout capture enabled, then

  * prints the layout statistics and the validation verdict per model;
  * renders the layout's arcs and nodes over the remeshed surface, so the
    scaffolding is visible next to the quads it produced.

Nodes are coloured by kind: red = singularity (extraordinary vertex), orange =
feature corner, green = boundary corner, small grey = T-junction.

Run through ``examples/run.sh`` so the built binaries are found::

    examples/run.sh examples/21_topology_layout.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import common as c  # noqa: E402

# Models chosen to show three different layout regimes: a CAD part whose layout
# is pinned to its creases, a smooth organic surface where the layout is pure
# separatrix structure, and a higher-genus mechanical part.
MODELS = [
    ("fandisk", 2000, "CAD — layout pinned to creases"),
    ("spot", 2000, "organic — pure separatrix structure"),
    ("rocker-arm", 2000, "genus 1 — handles constrain the layout"),
]

NODE_STYLE = {
    "singularity": ("#d62728", 34, "singularity"),
    "feature-corner": ("#ff7f0e", 22, "feature corner"),
    "boundary-corner": ("#2ca02c", 22, "boundary corner"),
    "t-junction": ("#8c8c8c", 6, "T-junction"),
    "regular": ("#8c8c8c", 6, "T-junction"),
}


_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS_DIR = os.path.join(_REPO, "examples", "models")


def find_cli() -> str:
    """Locates the built headless binary, or bails with how to build it."""
    for root, _dirs, files in os.walk(os.path.join(_REPO, "build")):
        if "cyberremesh" in files and os.access(os.path.join(root, "cyberremesh"), os.X_OK):
            return os.path.join(root, "cyberremesh")
    sys.exit(
        "cyberremesh CLI not found. Build it first:\n"
        "  cmake --preset cpu-headless && cmake --build --preset cpu-headless"
    )


def read_obj(path):
    """Minimal OBJ reader returning (positions, polygon faces, polylines)."""
    positions = []
    faces = []
    lines = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            if raw.startswith("v "):
                p = raw.split()
                positions.append((float(p[1]), float(p[2]), float(p[3])))
            elif raw.startswith("f "):
                idx = [int(tok.split("/")[0]) - 1 for tok in raw.split()[1:]]
                faces.append(idx)
            elif raw.startswith("l "):
                lines.append([int(tok) - 1 for tok in raw.split()[1:]])
    return np.asarray(positions, dtype=float), faces, lines


def build_layout(cli: str, model: str, target: int, workdir: str):
    """Remeshes `model` with layout capture on. Returns (report, out, layout)."""
    src = os.path.join(MODELS_DIR, f"{model}.obj")
    out = os.path.join(workdir, f"{model}_quads.obj")
    prefix = os.path.join(workdir, f"{model}_layout")
    env = dict(os.environ)
    # The global quantizer is what traces the layout; CYBER_ZR_LAYOUT makes the
    # tracer keep the geometry it otherwise discards and writes the layout out.
    env["CYBER_QC_BIMDF"] = "1"
    env["CYBER_ZR_LAYOUT"] = prefix
    proc = subprocess.run(
        [cli, "--input", src, "--output", out,
         "--quad-method", "quad-cover", "--target-quads", str(target)],
        env=env, capture_output=True, text=True, timeout=1800,
    )
    if proc.returncode != 0:
        return None, None, None
    report = f"{prefix}.json"
    if not os.path.exists(report):
        # No island reached the global quantizer, so no layout was traced.
        return None, out, None
    with open(report, "r", encoding="utf-8") as handle:
        return json.load(handle), out, f"{prefix}.obj"


def draw(ax, quads_path, layout_obj, layout_json, title):
    positions, faces, _ = read_obj(quads_path)
    c._draw(ax, {"positions": positions, "faces": faces}, title)
    # The remeshed surface is the backdrop; the layout is what we came to see.
    for coll in ax.collections:
        coll.set_alpha(0.18)
        coll.set_edgecolor((0.55, 0.55, 0.6, 0.35))

    lp, _lf, polylines = read_obj(layout_obj)
    for chain in polylines:
        pts = lp[chain]
        ax.plot(pts[:, 0], pts[:, 1], pts[:, 2], color="#1f4fd8", linewidth=1.1, zorder=5)

    seen = set()
    for node in layout_json["nodes"]:
        colour, size, label = NODE_STYLE.get(node["kind"], ("#8c8c8c", 6, "other"))
        x, y, z = node["p"]
        ax.scatter([x], [y], [z], s=size, c=colour, depthshade=False, zorder=6,
                   label=None if label in seen else label)
        seen.add(label)


def main() -> None:
    cli = find_cli()
    fig = plt.figure(figsize=(5.0 * len(MODELS), 5.4), dpi=130)
    rendered = 0
    with tempfile.TemporaryDirectory() as workdir:
        for col, (model, target, blurb) in enumerate(MODELS):
            src = os.path.join(MODELS_DIR, f"{model}.obj")
            if not os.path.exists(src):
                print(f"{model:<14} SKIP (model not present)")
                continue
            report, quads, layout_obj = build_layout(cli, model, target, workdir)
            if report is None:
                print(f"{model:<14} SKIP (no layout traced)")
                continue
            st = report["stats"]
            verdict = "valid" if report["valid"] else f"INVALID ({report['invalidReason']})"
            print(
                f"{model:<14} {verdict:<10} "
                f"nodes {st['nodes']:>4}  arcs {st['arcs']:>4}  patches {st['patches']:>4}  "
                f"singularities {st['singularities']:>4}  T-junctions {st['tJunctions']:>4}  "
                f"feature arcs {st['featureArcs']:>3}  non-quad patches {st['nonQuadPatches']:>3}  "
                f"non-closing {st['nonClosingPatches']:>2}  total index {st['totalIndex']:>3}"
            )
            ax = fig.add_subplot(1, len(MODELS), col + 1, projection="3d")
            draw(ax, quads, layout_obj, report,
                 f"{model} — {blurb}\n{st['singularities']} singularities, "
                 f"{st['arcs']} arcs, {st['patches']} patches")
            if rendered == 0:
                ax.legend(loc="lower left", fontsize=7, framealpha=0.85)
            rendered += 1

    if rendered == 0:
        sys.exit("no layout could be traced on any model")
    fig.suptitle(
        "Topology layout — the explicit loop scaffolding behind the quads "
        "(blue = layout arcs: separatrices and feature chains)",
        fontsize=13, fontweight="bold", color="#12233a", y=0.99,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    out = os.path.join(c.OUTPUT_DIR, "21_topology_layout.png")
    os.makedirs(c.OUTPUT_DIR, exist_ok=True)
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
