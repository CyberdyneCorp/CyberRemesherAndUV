#!/usr/bin/env python3
"""Layout robustness — how often the topology layout survives a folded map.

The relaxed seamless parameterization is *not* injective near high-distortion
cones: it folds locally, and it is not worth over-constraining the solve to stop
that (it costs field fidelity). So the tracer that discovers the topology layout
has to be robust to folds instead — a flipped UV triangle may degrade the
*geometry* of an arc, but it must never corrupt the layout *graph*.

This script measures exactly that, over a parameter sweep rather than a single
run, and prints the numbers whatever they are. Per model x target count x
adaptivity it records:

  ok            the tracer produced a T-mesh at all
  valid         the layout satisfies its hard invariants
  failedRays    separatrix launches abandoned at folded cones
  degraded      nodes whose sector winding the fold damaged
  repaired      fold-damaged fans recovered by signed-angle sectors
  rejected      orbits rejected during patch extraction (contained regions)
  nonClosing    patches whose boundary walk does not close
  nonQuad       patches that did not come out four-sided

and, for each rejected orbit, WHY it was rejected:

  sectors       a sector winding the classifier could not seat
  corners       corner count outside the 3..6 the templates handle
  sideMismatch  four corners but opposite relaxed sides disagree
  failedCone    touches a cone whose separatrix launches were abandoned

The two that matter for the fold-robustness gate are `rejected` and
`nonClosing`: those are the layout falling back rather than degrading. The
reason breakdown is what says whether a given model's containment is a FOLD
problem (sectors) or a COVERAGE problem (failedCone — typically an open
boundary the tracer was not allowed to terminate on).

Set ``CYBER_ZR_FOLD_REPAIR=1`` in the environment to measure the Phase B fold
repair against the same sweep.

Run through ``examples/run.sh``. Deliberately free of matplotlib for the text
report; the chart is drawn only when matplotlib is importable::

    examples/run.sh examples/22_layout_robustness.py
    examples/run.sh examples/22_layout_robustness.py --full
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS_DIR = os.path.join(_REPO, "examples", "models")
OUTPUT_DIR = os.environ.get(
    "CYBER_EXAMPLES_OUTPUT", os.path.join(_REPO, "examples", "output")
)

# The corpus is the real measurement, but `examples/models/` is git-ignored
# (09_test_models.py downloads it on demand), so CI has none. The procedural
# stand-ins cover the same regimes — a creased CAD solid that traces cleanly, an
# organic surface, and a genus-1 shape — so the sweep still exercises the tracer
# and still reports honest numbers, just on synthetic input.
CORPUS = ["spot", "fandisk", "rocker-arm", "cheburashka", "stanford-bunny", "cube"]
PROCEDURAL = [
    ("cube", lambda path: _common().cube_obj(path, subdiv=14)),
    ("bumpy sphere", lambda path: _common().bumpy_sphere_obj(path, rings=36, segments=54)),
    ("torus knot", lambda path: _common().torus_knot_obj(path)),
]
TARGETS = [1000, 2000, 4000, 8000]
ADAPTIVITY = [0.0, 0.5]

QUICK_TARGETS = [2000]
QUICK_ADAPTIVITY = [0.0]

# The tracer's own diagnostic line, and the layout report the ZRemesher stage
# writes next to it.
_TMESH = re.compile(
    r"\[qc\] bimdf tmesh: ok=(?P<ok>\d+).*?failedRays=(?P<failed>\d+) "
    r"degraded=(?P<degraded>\d+) repaired=(?P<repaired>\d+)\(proj (?P<projected>\d+)\) "
    r".*?excluded=(?P<excluded>\d+)"
)
_LAYOUT = re.compile(
    r"\[zr\] layout: valid=(?P<valid>\d+) .*?nonQuad=(?P<nonquad>\d+) "
    r"nonClosing=(?P<nonclosing>\d+)"
)
_WHY = re.compile(
    r"reject why: sectors=(?P<sectors>\d+) corners=(?P<corners>\d+) "
    r"sideMismatch=(?P<mismatch>\d+) failedCone=(?P<cone>\d+)"
)


def _common():
    """`common` is imported lazily: the text report must work without it."""
    import common
    return common


def resolve_sources(workdir):
    """(name, path) per sweep row — the corpus when complete, else procedural."""
    if all(os.path.exists(os.path.join(MODELS_DIR, f"{m}.obj")) for m in CORPUS):
        return [(m, os.path.join(MODELS_DIR, f"{m}.obj")) for m in CORPUS]
    print("corpus models absent (examples/models/ is downloaded on demand) — "
          "sweeping procedural stand-ins instead")
    out = []
    for name, make in PROCEDURAL:
        path = os.path.join(workdir, name.replace(" ", "_") + ".obj")
        make(path)
        out.append((name, path))
    return out


def find_cli() -> str:
    for root, _dirs, files in os.walk(os.path.join(_REPO, "build")):
        if "cyberremesh" in files and os.access(os.path.join(root, "cyberremesh"), os.X_OK):
            return os.path.join(root, "cyberremesh")
    sys.exit(
        "cyberremesh CLI not found. Build it first:\n"
        "  cmake --preset cpu-headless && cmake --build --preset cpu-headless"
    )


def run_case(cli: str, src: str, target: int, adaptivity: float, workdir: str):
    """One sweep cell. Returns a dict of counters, or None if the run failed.

    A solve may reach the tracer several times (the target-count calibration
    re-solves at adjusted spacings). The LAST report is the one whose layout the
    pipeline actually used, so that is what is recorded.
    """
    out = os.path.join(workdir, "out.obj")
    env = dict(os.environ)
    env["CYBER_QC_BIMDF"] = "1"
    env["CYBER_ZR_LAYOUT"] = "1"
    proc = subprocess.run(
        [cli, "--input", src, "--output", out, "--quad-method", "quad-cover",
         "--target-quads", str(target), "--adaptivity", str(adaptivity)],
        env=env, capture_output=True, text=True, timeout=3600,
    )
    if proc.returncode != 0:
        return None
    tmesh = list(_TMESH.finditer(proc.stderr))
    layout = list(_LAYOUT.finditer(proc.stderr))
    why = list(_WHY.finditer(proc.stderr))
    if not tmesh:
        return {"traced": False}
    t = tmesh[-1].groupdict()
    row = {
        "traced": True,
        "ok": int(t["ok"]),
        "failedRays": int(t["failed"]),
        "degraded": int(t["degraded"]),
        "repaired": int(t["repaired"]),
        "projected": int(t["projected"]),
        "rejected": int(t["excluded"]),
        "valid": 0,
        "nonQuad": 0,
        "nonClosing": 0,
        "whySectors": 0,
        "whyCorners": 0,
        "whyMismatch": 0,
        "whyFailedCone": 0,
    }
    if why:
        w = why[-1].groupdict()
        row["whySectors"] = int(w["sectors"])
        row["whyCorners"] = int(w["corners"])
        row["whyMismatch"] = int(w["mismatch"])
        row["whyFailedCone"] = int(w["cone"])
    if layout:
        loc = layout[-1].groupdict()
        row["valid"] = int(loc["valid"])
        row["nonQuad"] = int(loc["nonquad"])
        row["nonClosing"] = int(loc["nonclosing"])
    return row


def sweep(cli: str, targets, adaptivity, workdir):
    rows = []
    if True:
        for model, src in resolve_sources(workdir):
            for target in targets:
                for adapt in adaptivity:
                    row = run_case(cli, src, target, adapt, workdir)
                    if row is None:
                        print(f"  {model} @{target} a={adapt}: remesh FAILED")
                        continue
                    row.update(model=model, target=target, adaptivity=adapt)
                    rows.append(row)
                    if not row["traced"]:
                        print(f"  {model:<15} {target:>6} a={adapt:<4} "
                              f"no T-mesh reached (island did not route here)")
                        continue
                    print(
                        f"  {model:<15} {target:>6} a={adapt:<4} "
                        f"ok={row['ok']} valid={row['valid']} "
                        f"failedRays={row['failedRays']:>3} degraded={row['degraded']:>3} "
                        f"repaired={row['repaired']:>3} rejected={row['rejected']:>3} "
                        f"nonClosing={row['nonClosing']:>2} nonQuad={row['nonQuad']:>3}"
                        + (f"  why: sectors={row['whySectors']} corners={row['whyCorners']} "
                           f"sideMismatch={row['whyMismatch']} failedCone={row['whyFailedCone']}"
                           if row["rejected"] else "")
                    )
    return rows


def summarise(rows):
    traced = [r for r in rows if r.get("traced")]
    if not traced:
        return {}
    cells = len(traced)
    return {
        "cells": cells,
        "tmeshOkRate": sum(r["ok"] for r in traced) / cells,
        "layoutValidRate": sum(r["valid"] for r in traced) / cells,
        "cellsWithRejectedOrbits": sum(1 for r in traced if r["rejected"]),
        "cellsWithNonClosing": sum(1 for r in traced if r["nonClosing"]),
        "totalRejectedOrbits": sum(r["rejected"] for r in traced),
        "totalNonClosing": sum(r["nonClosing"] for r in traced),
        "totalFailedRays": sum(r["failedRays"] for r in traced),
        "totalDegradedNodes": sum(r["degraded"] for r in traced),
        "totalNonQuadPatches": sum(r["nonQuad"] for r in traced),
        "whySectors": sum(r["whySectors"] for r in traced),
        "whyCorners": sum(r["whyCorners"] for r in traced),
        "whyMismatch": sum(r["whyMismatch"] for r in traced),
        "whyFailedCone": sum(r["whyFailedCone"] for r in traced),
    }


def draw(rows, summary, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("  (matplotlib/numpy unavailable — text report only)")
        return
    traced = [r for r in rows if r.get("traced")]
    if not traced:
        return
    models = []
    for r in traced:
        if r["model"] not in models:
            models.append(r["model"])
    metrics = [("rejected", "rejected orbits", "#d62728"),
               ("nonClosing", "non-closing patches", "#ff7f0e"),
               ("failedRays", "abandoned launches", "#8c8c8c")]

    fig, axes = plt.subplots(1, 4, figsize=(19, 4.6), dpi=130)
    for ax, (key, label, colour) in zip(axes, metrics):
        heights = [sum(r[key] for r in traced if r["model"] == m) for m in models]
        ax.bar(range(len(models)), heights, color=colour)
        ax.set_xticks(range(len(models)))
        ax.set_xticklabels(models, rotation=30, ha="right", fontsize=8)
        ax.set_title(f"{label}\n(summed over the sweep)", fontsize=10)
        ax.grid(axis="y", alpha=0.25)
        for x, h in enumerate(heights):
            ax.text(x, h, str(h), ha="center", va="bottom", fontsize=8)
        ax.set_ylim(0, max(max(heights), 1) * 1.2)
    # Why the contained regions were contained: the breakdown is what says
    # which lever would move the bars on the left.
    ax = axes[3]
    reasons = [("whySectors", "sector winding", "#d62728"),
               ("whyCorners", "corner count", "#9467bd"),
               ("whyMismatch", "side mismatch", "#1f77b4"),
               ("whyFailedCone", "abandoned cone", "#8c8c8c")]
    bottom = np.zeros(len(models))
    for key, label, colour in reasons:
        heights = np.array([sum(r[key] for r in traced if r["model"] == m) for m in models],
                           dtype=float)
        ax.bar(range(len(models)), heights, bottom=bottom, color=colour, label=label)
        bottom += heights
    ax.set_xticks(range(len(models)))
    ax.set_xticklabels(models, rotation=30, ha="right", fontsize=8)
    ax.set_title("why those orbits were rejected\n(the lever that would move panel 1)",
                 fontsize=10)
    ax.legend(fontsize=7)
    ax.grid(axis="y", alpha=0.25)

    fig.suptitle(
        "Layout robustness under folded relaxed maps — "
        f"{summary['cells']} sweep cells, T-mesh ok {summary['tmeshOkRate']:.0%}, "
        f"layout valid {summary['layoutValidRate']:.0%}   "
        "(lower bars are better; zero is the Phase B gate)",
        fontsize=12, fontweight="bold", color="#12233a",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full", action="store_true",
                        help="the whole sweep (4 target counts x 2 adaptivity); the "
                             "default is one cell per model, which is what CI runs")
    parser.add_argument("--json", metavar="PATH", help="also write the raw rows as JSON")
    args = parser.parse_args()

    cli = find_cli()
    targets = TARGETS if args.full else QUICK_TARGETS
    adaptivity = ADAPTIVITY if args.full else QUICK_ADAPTIVITY
    with tempfile.TemporaryDirectory() as workdir:
        print(f"layout robustness sweep: {len(targets)} targets "
              f"x {len(adaptivity)} adaptivity per model")
        rows = sweep(cli, targets, adaptivity, workdir)
    summary = summarise(rows)
    if not summary:
        sys.exit("no sweep cell reached the tracer")

    print("\nsummary")
    for key, value in summary.items():
        shown = f"{value:.1%}" if key.endswith("Rate") else value
        print(f"  {key:<24} {shown}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump({"rows": rows, "summary": summary}, handle, indent=2)
        print(f"wrote {args.json}")
    draw(rows, summary, os.path.join(OUTPUT_DIR, "22_layout_robustness.png"))


if __name__ == "__main__":
    main()
