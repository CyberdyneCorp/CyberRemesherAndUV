#!/usr/bin/env python3
"""CAD multi-density sweep: ours (quad-cover default + --pure-quads) vs
QuadriFlow on box_sharp / cylinder / fandisk, target-quads in
{400,600,900,1400,2000,3000,4500}.

Measurement-only scratch script (docs/ROADMAP.md discipline: count-matched,
>=7 densities, repeats where cheap). Writes one CSV row per run to
tools/bench/cad_sweep_results.csv. Analysis/aggregation happens downstream.

Usage: python3 tools/bench/cad_sweep.py
"""

from __future__ import annotations

import csv
import subprocess
import sys
import time
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCH_DIR))
import mesh_metrics  # noqa: E402

REPO = BENCH_DIR.parent.parent
CACHE = REPO / ".bench-cache"
OUT_DIR = CACHE / "out" / "cad-sweep"
CSV_PATH = BENCH_DIR / "cad_sweep_results.csv"
CYBER = REPO / "build" / "cpu-headless" / "apps" / "cli" / "cyberremesh"
QUADRIFLOW = Path(
    "/Users/leonardoaraujo/work/bench-baselines/quadriflow/build/quadriflow")

MESHES = {
    "box_sharp": CACHE / "generated" / "box_sharp.obj",
    "cylinder": CACHE / "generated" / "cylinder.obj",
    "fandisk": CACHE / "downloaded" / "fandisk.obj",
}
DENSITIES = [400, 600, 900, 1400, 2000, 3000, 4500]
REPEATS = 3
FAST_SECONDS = 5.0  # repeat only when a single run is under this
TIMEOUT = 900

ARMS = {
    "cyber": lambda i, o, n: [str(CYBER), "--input", str(i), "--output",
                              str(o), "--target-quads", str(n), "--quiet"],
    "cyber-pure": lambda i, o, n: [str(CYBER), "--input", str(i), "--output",
                                   str(o), "--target-quads", str(n),
                                   "--pure-quads", "--quiet"],
    "quadriflow": lambda i, o, n: [str(QUADRIFLOW), "-i", str(i), "-o",
                                   str(o), "-f", str(n)],
}

METRIC_KEYS = ["quads", "quad_ratio", "singularities", "angle_dev_mean",
               "feature_recall", "hausdorff_p99", "flow_loop_mean_len"]
FIELDS = ["mesh", "target_quads", "arm", "repeat", "ok", "seconds",
          *METRIC_KEYS, "error"]


def run_once(arm: str, mesh_path: Path, out_path: Path, target: int) -> dict:
    cmd = ARMS[arm](mesh_path, out_path, target)
    out_path.unlink(missing_ok=True)
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"timeout {TIMEOUT}s"}
    seconds = time.monotonic() - start
    if proc.returncode != 0 or not out_path.exists():
        detail = (proc.stderr or proc.stdout or "").strip()[-200:]
        return {"ok": False, "seconds": round(seconds, 3),
                "error": f"exit {proc.returncode}: {detail}"}
    metrics = mesh_metrics.compute_all(str(mesh_path), str(out_path))
    row = {"ok": True, "seconds": round(seconds, 3)}
    for key in METRIC_KEYS:
        row[key] = metrics.get(key)
    return row


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rows: list[dict] = []
    for mesh_name, mesh_path in MESHES.items():
        for target in DENSITIES:
            for arm in ARMS:
                repeats_done = 0
                while repeats_done < REPEATS:
                    out = OUT_DIR / f"{mesh_name}.{target}.{arm}.r{repeats_done}.obj"
                    result = run_once(arm, mesh_path, out, target)
                    row = {"mesh": mesh_name, "target_quads": target,
                           "arm": arm, "repeat": repeats_done, **result}
                    rows.append(row)
                    print(f"{mesh_name:<10} n={target:<5} {arm:<11} "
                          f"r{repeats_done} "
                          f"{'ok' if result.get('ok') else 'FAIL'} "
                          f"quads={result.get('quads', '-')} "
                          f"sing={result.get('singularities', '-')} "
                          f"angle={result.get('angle_dev_mean', float('nan')) or float('nan'):.2f} "
                          f"{result.get('seconds', 0):.2f}s", flush=True)
                    repeats_done += 1
                    if not result.get("ok") or \
                            result.get("seconds", 0) >= FAST_SECONDS:
                        break  # 1 run for slow/failed cells
    with open(CSV_PATH, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row.get(k, "") for k in FIELDS})
    print(f"wrote {CSV_PATH} ({len(rows)} runs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
