#!/usr/bin/env python3
"""Record the ZRemesher symbolic injectability gate from CLI JSON reports.

The table is deliberately a measurement, not a quality score: a layout can be
valid yet have no route into the integer quantizer.  Every row validates the
exclusive-cause accounting before it is written, so a changed report contract
cannot silently become a new baseline.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

from corpus import downloaded_meshes, generated_meshes


REPO = Path(__file__).resolve().parents[2]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="cyberremesh executable built from this checkout")
    parser.add_argument("--cache", type=Path, default=REPO / ".bench-cache" / "injectability")
    parser.add_argument("--target-quads", type=int, default=2000)
    parser.add_argument("--corpus", choices=("generated", "downloaded"), default="generated",
                        help="offline fixtures or the pinned organic benchmark corpus")
    parser.add_argument("--csv", type=Path, default=REPO / "tools/bench/injectability.csv")
    parser.add_argument("--verify-output-reach", action="store_true",
                        help="force partial Bi-MDF pinning and require a changed output hash")
    return parser.parse_args()


def row_for(binary: Path, mesh: dict, target_quads: int, work: Path, env: dict | None = None) -> dict:
    work.mkdir(parents=True, exist_ok=True)
    output = work / (mesh["name"] + ".obj")
    report = work / (mesh["name"] + ".json")
    completed = subprocess.run(
        [str(binary), "--input", str(mesh["path"]), "--output", str(output),
         "--quad-method", "zremesher", "--target-quads", str(target_quads),
         "--report", str(report)],
        text=True, capture_output=True, check=False,
        env={**os.environ, **(env or {})},
    )
    if completed.returncode:
        raise RuntimeError("{} failed:\n{}".format(mesh["name"], completed.stderr[-2000:]))
    data = json.loads(report.read_text())
    zremesher = data.get("zremesher")
    if not zremesher or "injectability" not in zremesher:
        raise RuntimeError("{} did not emit injectability diagnostics".format(mesh["name"]))
    stats = zremesher["injectability"]
    attributed = sum(stats[key] for key in (
        "injectableArcs", "excludedArcs", "emptyRows", "latticeFreeRows",
        "fractionalCoefficientRows",
    ))
    if attributed != stats["arcs"]:
        raise RuntimeError("{}: arc causes {} do not reconcile with {} arcs".format(
            mesh["name"], attributed, stats["arcs"]))
    return {
        "model": mesh["name"], "target_quads": target_quads,
        "arcs": stats["arcs"], "injectable_arcs": stats["injectableArcs"],
        "excluded_arcs": stats["excludedArcs"], "empty_rows": stats["emptyRows"],
        "lattice_free_rows": stats["latticeFreeRows"],
        "fractional_coefficient_rows": stats["fractionalCoefficientRows"],
        "fractional_pivot_rows": stats["fractionalPivotRows"],
        "dropped_rows": stats["droppedRows"], "pivots": stats["pivots"],
        "clean_pivots": stats["cleanPivots"], "injected_pivots": stats["injectedPivots"],
        "optimum_deviation_energy": stats["optimumDeviationEnergy"],
        "realized_deviation_energy": stats["realizedDeviationEnergy"],
    }


def verify_output_reach(binary: Path, mesh: dict, target_quads: int, work: Path) -> None:
    """Prove a feasible pinning perturbation reaches the mesh, not just a report."""
    baseline = row_for(binary, mesh, target_quads, work / "baseline")
    forced = row_for(binary, mesh, target_quads, work / "forced", {"CYBER_QC_BIMDF": "force"})
    baseline_obj = work / "baseline" / (mesh["name"] + ".obj")
    forced_obj = work / "forced" / (mesh["name"] + ".obj")
    hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in (baseline_obj, forced_obj)]
    if baseline["injected_pivots"] != 0 or forced["injected_pivots"] == 0:
        raise RuntimeError("{} did not change the feasible pin assignment".format(mesh["name"]))
    if hashes[0] == hashes[1]:
        raise RuntimeError("{} changed pins but not the output mesh".format(mesh["name"]))
    print("output reach: {} injected pivots {} -> {}, sha256 {} -> {}".format(
        mesh["name"], baseline["injected_pivots"], forced["injected_pivots"],
        hashes[0][:12], hashes[1][:12]))


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit("missing executable: {}".format(binary))
    meshes = (generated_meshes(args.cache / "meshes") if args.corpus == "generated"
              else downloaded_meshes(args.cache / "meshes"))
    with tempfile.TemporaryDirectory(prefix="cyber-injectability-") as temporary:
        work = Path(temporary)
        rows = [row_for(binary, mesh, args.target_quads, work / "measure") for mesh in meshes]
        if args.verify_output_reach:
            candidate = next((mesh for mesh, row in zip(meshes, rows)
                              if row["injectable_arcs"] > 0), None)
            if candidate is None:
                raise RuntimeError("no corpus mesh has a feasible arc to perturb")
            verify_output_reach(binary, candidate, args.target_quads, work / "reach")
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print("{model}: {injectable_arcs}/{arcs} injectable, excluded={excluded_arcs}, "
              "empty={empty_rows}, lattice-free={lattice_free_rows}, "
              "fractional={fractional_coefficient_rows}".format(**row))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
