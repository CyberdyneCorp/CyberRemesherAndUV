#!/usr/bin/env python3
"""Bi-MDF guided-rounding default-flip A/B (openspec bimdf-quantization task 5).

Runs CYBER_QC_BIMDF unset ("off") vs =guided over a corpus x densities x
arms on a given cyberremesh binary, count-matched per docs/ROADMAP.md
discipline: the guided run must land within --match-tol of the off run's
ACHIEVED quad count, re-requesting with a scaled target when it does not.

The flag lives in the native seamless solver, so the evaluation runs on both
backends: the Geogram build (native routes crease-heavy meshes only) and the
no-Geogram build (native is the only path — the ubuntu CI / stock backend).
Engagement is parsed from the solver's [qc] bimdf stderr lines so every row
records whether guided actually steered, was refused by the health gates, or
never reached the flag at all.

Usage:
  python3 tools/bench/bimdf_ab.py --binary <cyberremesh> --backend geo \
      --models spot,fandisk --densities 500,1000 --arms mixed,pure \
      --csv tools/bench/bimdf_ab_results.csv
  python3 tools/bench/bimdf_ab.py ... --repeat 3 --states off   # noise floor
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCH_DIR))
import mesh_metrics  # noqa: E402

REPO = BENCH_DIR.parent.parent
MODELS_DIR = REPO / "examples" / "models"
TIMEOUT = 1800

METRIC_KEYS = [
    "quads", "quad_ratio", "singularities", "singularity_ratio",
    "angle_dev_mean", "angle_dev_median", "angle_dev_p95",
    "flow_loop_mean_len", "flow_turning_mean", "hausdorff_p99",
    "chamfer_mean", "feature_recall", "edge_length_cv",
]
FIELDS = [
    "backend", "mesh", "density", "arm", "state", "repeat", "request",
    "ok", "seconds", "engagement", "count_matched", *METRIC_KEYS, "error",
]

# Sweep-local env: strip every CYBER_* toggle so each run sees exactly the
# default configuration plus the one flag under test.
BASE_ENV = {k: v for k, v in os.environ.items() if not k.startswith("CYBER_")}


def classify_engagement(stderr: str) -> str:
    """What the flag actually did in this run, from the solver's own trace."""
    if "bimdf guided: refused" in stderr:
        return "refused"  # health gate refused -> pure greedy
    steer = re.search(r"bimdf guided: steerSolve ok=(\d)", stderr)
    inject = re.search(r"bimdf realized: .*injected=(\d+)", stderr)
    if steer:
        n = inject.group(1) if inject else "?"
        return f"steered(injected={n})"
    if "bimdf tmesh:" in stderr:
        n = inject.group(1) if inject else "0"
        return f"tmesh-only(injected={n})"
    return "flag-unread"  # native solver never ran (vendored path)


def run_once(binary: Path, mesh_path: Path, out_path: Path, request: int,
             arm: str, state: str) -> tuple[dict, str]:
    cmd = [str(binary), "--input", str(mesh_path), "--output", str(out_path),
           "--target-quads", str(request), "--quiet"]
    if arm == "pure":
        cmd.append("--pure-quads")
    env = dict(BASE_ENV)
    if state != "off":
        env["CYBER_QC_BIMDF"] = state
    out_path.unlink(missing_ok=True)
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"timeout {TIMEOUT}s"}, ""
    seconds = time.monotonic() - start
    if proc.returncode != 0 or not out_path.exists():
        detail = (proc.stderr or proc.stdout or "").strip()[-200:]
        return {"ok": False, "seconds": round(seconds, 2),
                "error": f"exit {proc.returncode}: {detail}"}, proc.stderr
    row = {"ok": True, "seconds": round(seconds, 2)}
    metrics = mesh_metrics.compute_all(str(mesh_path), str(out_path))
    for key in METRIC_KEYS:
        row[key] = metrics.get(key)
    return row, proc.stderr


def run_cell(binary: Path, mesh: str, density: int, arm: str, backend: str,
             out_dir: Path, writer, csv_file, states: list[str],
             repeat: int, match_tol: float) -> None:
    mesh_path = MODELS_DIR / f"{mesh}.obj"
    off_quads = None
    for state in states:
        for rep in range(repeat):
            request = density
            attempts = 3
            while True:
                out_path = out_dir / f"{mesh}.{density}.{arm}.{state}.obj"
                row, stderr = run_once(binary, mesh_path, out_path, request,
                                       arm, state)
                matched = True
                if (row.get("ok") and state != "off" and off_quads
                        and attempts > 1):
                    got = row["quads"]
                    if got and abs(got - off_quads) / off_quads > match_tol:
                        # Re-request toward the off arm's achieved count.
                        request = max(50, int(round(request * off_quads / got)))
                        attempts -= 1
                        continue
                if (row.get("ok") and state != "off" and off_quads):
                    matched = (abs(row["quads"] - off_quads) / off_quads
                               <= match_tol)
                break
            if row.get("ok") and state == "off" and rep == 0:
                off_quads = row["quads"]
            record = {"backend": backend, "mesh": mesh, "density": density,
                      "arm": arm, "state": state, "repeat": rep,
                      "request": request,
                      "engagement": classify_engagement(stderr),
                      "count_matched": matched, **row}
            writer.writerow({k: record.get(k) for k in FIELDS})
            csv_file.flush()
            print(f"{backend} {mesh}@{density} {arm} {state} rep{rep}: "
                  f"quads={row.get('quads')} sing={row.get('singularities')} "
                  f"{record['engagement']} {row.get('seconds')}s",
                  flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--backend", required=True, help="label: geo | nogeo")
    ap.add_argument("--models", required=True)
    ap.add_argument("--densities", required=True)
    ap.add_argument("--arms", default="mixed,pure")
    ap.add_argument("--states", default="off,guided")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--match-tol", type=float, default=0.02)
    ap.add_argument("--csv", type=Path,
                    default=BENCH_DIR / "bimdf_ab_results.csv")
    ap.add_argument("--out-dir", type=Path,
                    default=REPO / ".bench-cache" / "out" / "bimdf-ab")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    new_file = not args.csv.exists()
    with open(args.csv, "a", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS)
        if new_file:
            writer.writeheader()
        for mesh in args.models.split(","):
            for density in (int(d) for d in args.densities.split(",")):
                for arm in args.arms.split(","):
                    run_cell(args.binary, mesh, density, arm, args.backend,
                             args.out_dir, writer, fh,
                             args.states.split(","), args.repeat,
                             args.match_tol)
    return 0


if __name__ == "__main__":
    sys.exit(main())
