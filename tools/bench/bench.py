#!/usr/bin/env python3
"""Remeshing benchmark harness (task 5.9).

Runs solvers over a corpus, computes the metrics in mesh_metrics.py, and
maintains recorded baselines so every solver change lands with a scoreboard.

  run     benchmark solvers over a corpus, write results JSON, print a table
  record  run cyber over the generated corpus and (re)write baselines
  check   run cyber over the generated corpus and compare against baselines
          (nonzero exit on regression) — this is what ctest invokes

External baselines (QuadriFlow, Instant Meshes, AutoRemesher, quadwild-bimdf)
are user-provided binaries resolved from env vars in solvers.json; they are
never vendored (quadwild is GPL-3).
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

import corpus
import mesh_metrics

BENCH_DIR = Path(__file__).resolve().parent
REPO = BENCH_DIR.parent.parent
DEFAULT_CACHE = REPO / ".bench-cache"
BASELINES = REPO / "tests" / "bench" / "baselines.json"
DEFAULT_CYBER = REPO / "build" / "cpu-headless" / "apps" / "cli" / "cyberremesh"

# metric -> (kind, tolerance, direction). direction: 'both' fails on any drift
# beyond tolerance; 'increase' fails only when the metric got worse (higher).
CHECKED_METRICS = {
    "quad_ratio": ("abs", 0.05, "decrease"),
    "singularities": ("rel", 0.25, "increase"),
    "hausdorff_p99": ("rel", 0.30, "increase"),
    "chamfer_mean": ("rel", 0.30, "increase"),
    "angle_dev_mean": ("rel", 0.25, "increase"),
    "flow_turning_mean": ("rel", 0.25, "increase"),
    "feature_recall": ("abs", 0.10, "decrease"),
}


def load_solvers(cyber_binary: Path) -> dict:
    config = json.loads((BENCH_DIR / "solvers.json").read_text())["solvers"]
    resolved = {}
    for name, spec in config.items():
        if name in ("cyber", "cyber-pure"):
            exe = cyber_binary
        else:
            import os
            env_var = spec.get("exe_env", "")
            if os.environ.get(env_var):
                exe = Path(os.environ[env_var])
            elif spec.get("exe"):
                exe = Path(spec["exe"])
            else:
                exe = None
        if exe is None or not Path(exe).exists():
            continue
        resolved[name] = {**spec, "exe": str(exe)}
    return resolved


def run_solver(spec: dict, input_path: Path, output_path: Path, faces: int,
               timeout: int) -> dict:
    cmd = [arg.format(exe=spec["exe"], input=input_path, output=output_path,
                      faces=faces) for arg in spec["cmd"]]
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"timeout after {timeout}s"}
    seconds = time.monotonic() - start
    suffix = spec.get("output_from_input_suffix")
    if suffix:  # solvers that name their own output next to the input
        produced = input_path.with_name(input_path.stem + suffix)
        if produced.exists():
            produced.replace(output_path)
    # Some tools (quadwild) exit nonzero even on success; for those, the
    # produced output file is the success signal.
    failed = not output_path.exists() or (proc.returncode != 0 and
                                          not spec.get("ignore_exit_code"))
    if failed:
        detail = (proc.stderr or proc.stdout or "").strip()[-400:]
        return {"ok": False,
                "error": f"exit {proc.returncode}, cmd: {shlex.join(cmd)}\n{detail}"}
    return {"ok": True, "seconds": round(seconds, 3)}


def benchmark(meshes: list[dict], solvers: dict, out_dir: Path, samples: int,
              timeout: int) -> list[dict]:
    out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for mesh in meshes:
        for name, spec in solvers.items():
            output = out_dir / f"{mesh['name']}.{name}.obj"
            row = {"mesh": mesh["name"], "solver": name,
                   "target_quads": mesh["target_quads"]}
            outcome = run_solver(spec, mesh["path"], output, mesh["target_quads"],
                                 timeout)
            row.update(outcome)
            if outcome["ok"]:
                row["metrics"] = mesh_metrics.compute_all(
                    str(mesh["path"]), str(output), samples=samples)
            print(format_row(row))
            results.append(row)
    return results


def format_row(row: dict) -> str:
    if not row.get("ok"):
        return f"{row['mesh']:<12} {row['solver']:<14} FAILED: {row['error']}"
    m = row["metrics"]
    recall = m.get("feature_recall")
    recall_str = f"{recall:.2f}" if recall is not None else "  - "
    flow = m.get("flow_turning_mean")
    flow_str = f"{flow:5.1f}" if flow is not None else "    -"
    return (f"{row['mesh']:<12} {row['solver']:<14} "
            f"quads {m['quads']:>6} ({m['quad_ratio']:.2f}) "
            f"sing {m['singularities']:>4} "
            f"haus {m['hausdorff_p99']:.4f} "
            f"angle {m['angle_dev_mean']:5.1f} "
            f"flow {flow_str} "
            f"recall {recall_str} "
            f"{row['seconds']:6.1f}s")


def _violates(kind: str, tolerance: float, direction: str,
              baseline: float, current: float) -> bool:
    delta = current - baseline
    if direction == "increase" and delta <= 0:
        return False
    if direction == "decrease" and delta >= 0:
        return False
    if kind == "abs":
        return abs(delta) > tolerance
    scale = max(abs(baseline), 1e-9)
    return abs(delta) / scale > tolerance


# ctest's SKIP_RETURN_CODE: "this configuration is not the one the baselines
# describe", which is neither a pass nor a regression.
SKIP_EXIT = 77


def solver_identity(cyber_binary: Path) -> str:
    """The seamless-UV solver the binary carries, from `cyberremesh --version`.

    Which solver a build routes to is a BUILD option (-DCYBER_WITH_QUADCOVER),
    and the two produce genuinely different quads, so a baseline recorded on one
    is not a gate on the other. Empty when the binary is too old to report it.
    """
    try:
        out = subprocess.run([str(cyber_binary), "--version"], capture_output=True,
                             text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    for line in out.splitlines():
        if line.startswith("seamless-uv-solver "):
            return line.split(" ", 1)[1].strip()
    return ""


def check_against_baselines(results: list[dict]) -> int:
    baselines = json.loads(BASELINES.read_text())["results"]
    failures = 0
    for row in results:
        key = row["mesh"]
        if not row.get("ok"):
            print(f"CHECK FAIL {key}: solver failed: {row['error']}")
            failures += 1
            continue
        base = baselines.get(key)
        if base is None:
            print(f"CHECK WARN {key}: no recorded baseline (run `bench.py record`)")
            continue
        for metric, (kind, tol, direction) in CHECKED_METRICS.items():
            baseline_value = base.get(metric)
            current = row["metrics"].get(metric)
            if baseline_value is None or current is None:
                continue
            if _violates(kind, tol, direction, baseline_value, current):
                print(f"CHECK FAIL {key}.{metric}: baseline {baseline_value:.4g} "
                      f"-> {current:.4g} (tol {kind} {tol}, {direction})")
                failures += 1
    return failures


def record_baselines(results: list[dict], solver: str) -> None:
    payload = {
        "comment": "Recorded by tools/bench/bench.py record on the generated "
                   "corpus. Regenerate deliberately after intentional solver "
                   "changes; the diff is the review artifact.",
        "solver": solver,
        "results": {
            row["mesh"]: row["metrics"] for row in results if row.get("ok")
        },
    }
    BASELINES.parent.mkdir(parents=True, exist_ok=True)
    BASELINES.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"wrote {BASELINES} ({len(payload['results'])} meshes, solver {solver})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["run", "record", "check"])
    parser.add_argument("--corpus", choices=["generated", "downloaded", "all"],
                        default="generated")
    parser.add_argument("--solvers", default="all",
                        help="comma-separated solver names (default: all resolved)")
    parser.add_argument("--cyber-binary", type=Path, default=DEFAULT_CYBER)
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--samples", type=int, default=mesh_metrics.DEFAULT_SAMPLES)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--results", type=Path,
                        help="write full results JSON to this path")
    args = parser.parse_args()

    meshes: list[dict] = []
    if args.corpus in ("generated", "all"):
        meshes += corpus.generated_meshes(args.cache_dir / "generated")
    if args.corpus in ("downloaded", "all"):
        meshes += corpus.downloaded_meshes(args.cache_dir / "downloaded")

    solvers = load_solvers(args.cyber_binary)
    if args.command in ("record", "check"):
        solvers = {k: v for k, v in solvers.items() if k == "cyber"}
        if args.corpus != "generated":
            print("record/check always use the generated corpus", file=sys.stderr)
            meshes = corpus.generated_meshes(args.cache_dir / "generated")
    elif args.solvers != "all":
        wanted = set(args.solvers.split(","))
        missing = wanted - set(solvers)
        if missing:
            print(f"unresolved solvers (set their exe env vars): {sorted(missing)}",
                  file=sys.stderr)
            return 2
        solvers = {k: v for k, v in solvers.items() if k in wanted}
    if "cyber" in solvers and not Path(solvers["cyber"]["exe"]).exists():
        print(f"cyberremesh not found at {solvers['cyber']['exe']}", file=sys.stderr)
        return 2
    if not solvers:
        print("no solvers resolved", file=sys.stderr)
        return 2

    # A `check` against baselines recorded on a DIFFERENT solver build is not a
    # regression report — it is a comparison of two different algorithms, and it
    # failed six metrics on every CYBER_WITH_QUADCOVER=OFF configuration
    # (including the portable leg of the nightly sanitizer lane, which runs the
    # whole ctest set). Skip instead, and say which build would be gated.
    if args.command == "check":
        recorded = json.loads(BASELINES.read_text()).get("solver", "")
        current = solver_identity(args.cyber_binary)
        if recorded and current and recorded != current:
            print(f"bench check SKIPPED: baselines were recorded on the "
                  f"'{recorded}' build, this binary is '{current}'. Configure "
                  f"with the same -DCYBER_WITH_QUADCOVER setting to gate on "
                  f"them, or re-record deliberately.")
            return SKIP_EXIT

    results = benchmark(meshes, solvers, args.cache_dir / "out", args.samples,
                        args.timeout)
    if args.results:
        args.results.write_text(json.dumps(results, indent=2) + "\n")

    if args.command == "record":
        record_baselines(results, solver_identity(args.cyber_binary))
        return 0
    if args.command == "check":
        failures = check_against_baselines(results)
        print("bench check:", "OK" if failures == 0 else f"{failures} failure(s)")
        return 1 if failures else 0
    return 0 if all(r.get("ok") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
