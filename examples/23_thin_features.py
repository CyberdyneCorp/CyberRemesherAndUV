#!/usr/bin/env python3
"""Thin features — do plates, fins and tubes survive a coarse remesh?

Curvature does not tell you whether a structure is thin. A fin, an ear, a finger,
a strap or a narrow bracket can be perfectly smooth and still be one edge length
across, and a target edge longer than the feature is thick will pinch it shut —
the two sides get bridged into one sheet and the feature is simply gone, with no
warning and no obviously-broken output.

This measures that directly, rather than through a proxy. Each fixture is a
shape with a known thin dimension; the check is whether the remesh still has two
distinct surfaces across the thin part, and how far the output drifted from the
input.

  bridged      output vertices sitting in the gap between the two sides —
               where a collapsed feature puts them. 0 is the goal.
  sides        vertices on each side of the thin dimension; both must stay
               populated, or one side was consumed.
  drift        largest distance from an output vertex to the input surface,
               as a fraction of the thin dimension.

Run through ``examples/run.sh``::

    examples/run.sh examples/23_thin_features.py
    CYBER_ZR_UNIFIED_SIZING=1 examples/run.sh examples/23_thin_features.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT_DIR = os.environ.get(
    "CYBER_EXAMPLES_OUTPUT", os.path.join(_REPO, "examples", "output")
)


def find_cli() -> str:
    for root, _dirs, files in os.walk(os.path.join(_REPO, "build")):
        if "cyberremesh" in files and os.access(os.path.join(root, "cyberremesh"), os.X_OK):
            return os.path.join(root, "cyberremesh")
    sys.exit("cyberremesh CLI not found — build it first (cmake --build --preset cpu-headless)")


def write_obj(path, verts, faces):
    with open(path, "w", encoding="utf-8") as fh:
        for v in verts:
            fh.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for f in faces:
            fh.write("f " + " ".join(str(i + 1) for i in f) + "\n")


def read_obj(path):
    verts, faces = [], []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("v "):
                p = line.split()
                verts.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                faces.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return verts, faces


def thin_plate(thickness, span=4.0, n=24):
    """A closed slab: two `span` x `span` sheets `thickness` apart, plus a rim.

    Closed on purpose — an open sheet has nothing to bridge to, so it would pass
    the test without testing anything.
    """
    verts, faces = [], []
    step = span / n

    def grid(z):
        base = len(verts)
        for j in range(n + 1):
            for i in range(n + 1):
                verts.append((i * step, j * step, z))
        return base

    lo = grid(0.0)
    hi = grid(thickness)

    def at(base, i, j):
        return base + j * (n + 1) + i

    for j in range(n):
        for i in range(n):
            faces.append([at(lo, i, j), at(lo, i, j + 1), at(lo, i + 1, j + 1), at(lo, i + 1, j)])
            faces.append([at(hi, i, j), at(hi, i + 1, j), at(hi, i + 1, j + 1), at(hi, i, j + 1)])
    # Rim, walking the border so the slab is closed.
    border = ([(i, 0) for i in range(n)] + [(n, j) for j in range(n)] +
              [(n - i, n) for i in range(n)] + [(0, n - j) for j in range(n)])
    for k in range(len(border)):
        i0, j0 = border[k]
        i1, j1 = border[(k + 1) % len(border)]
        faces.append([at(lo, i0, j0), at(hi, i0, j0), at(hi, i1, j1), at(lo, i1, j1)])
    return verts, faces


def thin_fin(thickness, height=3.0, length=4.0, n=20):
    """A blade standing off a base — the ear/fin shape, thin in one axis only."""
    verts, faces = [], []
    for side in (0.0, thickness):
        for j in range(n + 1):
            for i in range(n + 1):
                verts.append((i * length / n, j * height / n, side))
    per = (n + 1) * (n + 1)

    def at(side, i, j):
        return side * per + j * (n + 1) + i

    for j in range(n):
        for i in range(n):
            faces.append([at(0, i, j), at(0, i, j + 1), at(0, i + 1, j + 1), at(0, i + 1, j)])
            faces.append([at(1, i, j), at(1, i + 1, j), at(1, i + 1, j + 1), at(1, i, j + 1)])
    border = ([(i, 0) for i in range(n)] + [(n, j) for j in range(n)] +
              [(n - i, n) for i in range(n)] + [(0, n - j) for j in range(n)])
    for k in range(len(border)):
        i0, j0 = border[k]
        i1, j1 = border[(k + 1) % len(border)]
        faces.append([at(0, i0, j0), at(1, i0, j0), at(1, i1, j1), at(0, i1, j1)])
    return verts, faces


FIXTURES = [
    ("thin plate", thin_plate, 0.30),
    ("thinner plate", thin_plate, 0.15),
    ("thin fin", thin_fin, 0.25),
]


def measure(verts, thickness):
    """(bridged, low side, high side, drift) across the thin (z) dimension."""
    lo = sum(1 for v in verts if v[2] < thickness * 0.25)
    hi = sum(1 for v in verts if v[2] > thickness * 0.75)
    bridged = sum(1 for v in verts if thickness * 0.25 <= v[2] <= thickness * 0.75)
    drift = 0.0
    for v in verts:
        # Distance to the nearer of the two sheets, in z.
        drift = max(drift, min(abs(v[2]), abs(v[2] - thickness)))
    return bridged, lo, hi, drift / thickness if thickness else 0.0


def main() -> None:
    cli = find_cli()
    print(f"{'fixture':<15} {'target/thick':>12} {'quads':>7} {'bridged':>8} "
          f"{'lo/hi sides':>13} {'drift':>7}  verdict")
    failures = 0
    with tempfile.TemporaryDirectory() as work:
        for name, make, thickness in FIXTURES:
            verts, faces = make(thickness)
            src = os.path.join(work, name.replace(" ", "_") + ".obj")
            write_obj(src, verts, faces)
            # A target edge deliberately COARSER than the feature is thick:
            # this is the case a uniform sizing field destroys.
            target_edge = thickness * 3.0
            # Roughly one quad per target_edge^2 over the surface area.
            area = 2 * 4.0 * 4.0  # both sheets; the rim is negligible at these ratios
            quads = max(200, int(area / (target_edge * target_edge)))
            out = os.path.join(work, "out.obj")
            proc = subprocess.run(
                [cli, "--input", src, "--output", out, "--quad-method", "zremesher",
                 "--target-quads", str(quads), "--quiet"],
                capture_output=True, text=True, timeout=1800,
            )
            if proc.returncode != 0 or not os.path.exists(out):
                print(f"{name:<15} remesh FAILED")
                failures += 1
                continue
            ov, of = read_obj(out)
            bridged, lo, hi, drift = measure(ov, thickness)
            survived = bridged == 0 and lo > 0 and hi > 0
            failures += 0 if survived else 1
            print(f"{name:<15} {target_edge / thickness:>11.1f}x {len(of):>7} {bridged:>8} "
                  f"{lo:>6}/{hi:<6} {drift:>6.2f}  "
                  f"{'survived' if survived else 'COLLAPSED'}")

    print()
    if failures:
        print(f"{failures} of {len(FIXTURES)} fixtures did not survive — a target edge "
              f"coarser than the feature is thick pinched it shut.")
    else:
        print("every thin feature survived a target edge coarser than its thickness.")
    print("re-run with CYBER_ZR_UNIFIED_SIZING=1 to compare against the unified sizing\n"
          "field, which measured WORSE here and is off by default.")


if __name__ == "__main__":
    main()
