#!/usr/bin/env python3
"""Acceptance gate for the opt-in complete organic half-lattice guide."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from corpus import generated_meshes
from mesh_metrics import compute_all


METRICS = {
    "quad_ratio": (0.05, "decrease"),
    "singularities": (0.25, "increase-relative"),
    "singularity_ratio": (0.25, "increase-relative"),
    "hausdorff_p99": (0.30, "increase-relative"),
    "chamfer_mean": (0.30, "increase-relative"),
    "angle_dev_mean": (0.25, "increase-relative"),
    "angle_dev_median": (0.25, "increase-relative"),
    "edge_length_cv": (0.25, "increase-relative"),
    "flow_turning_mean": (0.25, "increase-relative"),
    "feature_recall": (0.10, "decrease"),
    "realized_deviation_energy": (0.30, "increase-relative"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=10_000)
    return parser.parse_args()


def run(binary: Path, mesh: dict, target: int, output: Path, project: bool) -> tuple[dict, str]:
    report = output.with_suffix(".json")
    env = os.environ.copy()
    if project:
        env["CYBER_ZR_HALF_LATTICE"] = "project"
    completed = subprocess.run(
        [str(binary), "--input", str(mesh["path"]), "--output", str(output),
         "--quad-method", "zremesher", "--target-quads", str(target), "--report", str(report)],
        capture_output=True, text=True, env=env, check=False)
    if completed.returncode:
        raise RuntimeError(f"{mesh['name']} failed: {completed.stderr[-1000:]}")
    zremesher = json.loads(report.read_text())["zremesher"]
    stats = zremesher["injectability"]
    stats["_layouts_valid"] = zremesher["layoutsValid"]
    return stats, hashlib.sha256(output.read_bytes()).hexdigest()


def regression(baseline: dict, guided: dict) -> list[str]:
    failures = []
    for name, (tolerance, direction) in METRICS.items():
        before, after = baseline[name], guided[name]
        if before is None or after is None:
            continue
        if direction == "decrease":
            bad = after < before - tolerance
        else:
            limit = before * (1.0 + tolerance) if before else tolerance
            bad = after > limit
        if bad:
            failures.append(f"{name}: {before} -> {after}")
    return failures


def measure_corpus(binary: Path, meshes: list[dict], target: int | None, work: Path,
                   samples: int) -> list[str]:
    failures = []
    for mesh in meshes:
        actual_target = target if target is not None else mesh["target_quads"]
        base_out = work / "baseline" / mesh["name"] / "mesh.obj"
        guide_out = work / "guide" / mesh["name"] / "mesh.obj"
        base_out.parent.mkdir(parents=True, exist_ok=True)
        guide_out.parent.mkdir(parents=True, exist_ok=True)
        base_stats, base_hash = run(binary, mesh, actual_target, base_out, False)
        guide_stats, guide_hash = run(binary, mesh, actual_target, guide_out, True)
        base_metrics = compute_all(str(mesh["path"]), str(base_out), samples=samples)
        guide_metrics = compute_all(str(mesh["path"]), str(guide_out), samples=samples)
        base_metrics["realized_deviation_energy"] = base_stats["realizedDeviationEnergy"]
        guide_metrics["realized_deviation_energy"] = guide_stats["realizedDeviationEnergy"]
        if guide_stats["_layouts_valid"] == 0:
            failures.append(f"{mesh['name']}@{actual_target}: guided layout is invalid")
        if not guide_metrics["valid"]:
            failures.append(f"{mesh['name']}@{actual_target}: guided output is invalid")
        failures.extend(f"{mesh['name']}@{actual_target}: {failure}"
                        for failure in regression(base_metrics, guide_metrics))
        if mesh["name"] == "sphere" and actual_target == 100:
            if guide_stats["halfLatticeGuidedProjectedComponents"] == 0:
                failures.append("sphere@100: no complete projected component was guided")
            if base_hash == guide_hash:
                failures.append("sphere@100: component guide did not affect output")
            repeat_out = work / "repeat" / mesh["name"] / "mesh.obj"
            repeat_out.parent.mkdir(parents=True, exist_ok=True)
            _, repeat_hash = run(binary, mesh, actual_target, repeat_out, True)
            if guide_hash != repeat_hash:
                failures.append("sphere@100: guided output is not deterministic")
        print(f"{mesh['name']}@{actual_target}: projected "
              f"{guide_stats['halfLatticeGuidedProjectedComponents']}, "
              f"hash {base_hash[:12]} -> {guide_hash[:12]}")
    return failures


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit(f"missing executable: {binary}")
    with tempfile.TemporaryDirectory(prefix="cyber-half-lattice-gate-") as temporary:
        work = Path(temporary)
        meshes = generated_meshes(work / "meshes")
        failures = measure_corpus(binary, meshes, 100, work / "target-100", args.samples)
        failures.extend(measure_corpus(binary, meshes, None, work / "standard", args.samples))
    if failures:
        raise SystemExit("half-lattice component gate failed:\n" + "\n".join(failures))
    print("half-lattice component gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
