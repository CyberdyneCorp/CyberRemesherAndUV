#!/usr/bin/env python3
"""Flow guides and painted density — the exit-gate measurement.

Draws a guide across a limb/ear junction on a real corpus mesh, remeshes with
and without it at matched density, and reports the MEAN ANGULAR DEVIATION of
the extracted quad edge directions from the guide tangent inside the guide's
influence radius (folded mod 90 degrees, so a random field averages 22.5).

The proposal's exit gate is <= 15 degrees mean in-radius deviation. This script
is the artifact the reported number comes from; it prints whatever it measures,
met or not.

Run through ``examples/run.sh`` so the C ABI library is found. Deliberately
free of matplotlib/numpy so it also runs on a bare headless box.
"""

from __future__ import annotations

import math
import os
import sys
import tempfile

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO, "python", "cyberremesh"))

from cyberremesh import FlowGuide, Mesh, RemeshParams, remesh  # noqa: E402

MODELS = os.path.join(_REPO, "examples", "models")


def load_obj(path):
    positions = []
    faces = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("v "):
                parts = line.split()
                positions.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif line.startswith("f "):
                idx = [int(tok.split("/")[0]) - 1 for tok in line.split()[1:]]
                faces.append(idx)
    return positions, faces


def bounds(positions):
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    return lo, hi


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 0 else (0.0, 0.0, 0.0)


def point_segment_distance(p, a, b):
    ab = sub(b, a)
    len2 = dot(ab, ab)
    t = 0.0 if len2 <= 1e-20 else max(0.0, min(1.0, dot(sub(p, a), ab) / len2))
    q = (a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t)
    return math.sqrt(dot(sub(p, q), sub(p, q)))


def mean_in_radius_deviation(positions, faces, guide):
    """Mean min-angle-mod-90 between the guide tangent and quad edge directions."""
    total = 0.0
    count = 0
    segments = list(zip(guide.points, guide.points[1:]))
    for face in faces:
        if len(face) < 3:
            continue
        pts = [positions[i] for i in face]
        centroid = tuple(sum(p[k] for p in pts) / len(pts) for k in range(3))
        best_seg = None
        best_d = guide.radius
        for a, b in segments:
            d = point_segment_distance(centroid, a, b)
            if d < best_d:
                best_d = d
                best_seg = (a, b)
        if best_seg is None:
            continue
        n = norm(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])))
        tangent = norm(sub(best_seg[1], best_seg[0]))
        t = norm(sub(tangent, tuple(n[k] * dot(n, tangent) for k in range(3))))
        if dot(t, t) < 1e-12:
            continue
        best = 90.0
        for k in range(len(pts)):
            e = sub(pts[(k + 1) % len(pts)], pts[k])
            e = sub(e, tuple(n[j] * dot(n, e) for j in range(3)))
            if dot(e, e) < 1e-16:
                continue
            e = norm(e)
            dev = math.degrees(math.acos(min(1.0, abs(dot(e, t)))))
            best = min(best, min(dev, abs(90.0 - dev)))
        total += best
        count += 1
    return (total / count if count else float("nan")), count


def sample_guide_on_surface(positions, axis, other, count, radius_fraction):
    """A stroke running along `axis` through the middle of the model's bulk.

    Points are taken from actual input vertices (nearest to a swept line), so
    the stroke sits on the surface without needing a projection step here.
    """
    lo, hi = bounds(positions)
    extent = [hi[i] - lo[i] for i in range(3)]
    diag = math.sqrt(sum(e * e for e in extent))
    mid = [(lo[i] + hi[i]) * 0.5 for i in range(3)]
    points = []
    for k in range(count):
        t = (k + 0.5) / count
        target = list(mid)
        target[axis] = lo[axis] + extent[axis] * (0.25 + 0.5 * t)
        target[other] = hi[other]  # ride the silhouette rather than cut through
        best = min(positions, key=lambda p: sum((p[i] - target[i]) ** 2 for i in range(3)))
        if not points or best != points[-1]:
            points.append(best)
    return FlowGuide(points=points, strength=1.0, radius=diag * radius_fraction)


def run_model(name, quads, axis, other, radius_fraction):
    path = os.path.join(MODELS, name)
    if not os.path.exists(path):
        print(f"  {name}: not present, skipped")
        return None
    positions, _ = load_obj(path)
    guide = sample_guide_on_surface(positions, axis, other, 12, radius_fraction)

    params = RemeshParams(target_quad_count=quads, adaptivity=0.0)
    tmp = tempfile.mkdtemp()
    with Mesh.load_obj(path) as source:
        unguided = remesh(source, params)
        guided = remesh(source, params, guides=[guide])
        unguided.save_obj(os.path.join(tmp, "unguided.obj"))
        guided.save_obj(os.path.join(tmp, "guided.obj"))
        for w in guided.guidance_warnings:
            print(f"    warning: {w}")
    u_pos, u_faces = load_obj(os.path.join(tmp, "unguided.obj"))
    g_pos, g_faces = load_obj(os.path.join(tmp, "guided.obj"))

    dev_u, n_u = mean_in_radius_deviation(u_pos, u_faces, guide)
    dev_g, n_g = mean_in_radius_deviation(g_pos, g_faces, guide)
    gate = "MET" if dev_g <= 15.0 else "NOT MET"
    print(
        f"  {name:<20} r={guide.radius:6.4f}  unguided {dev_u:6.2f}  guided {dev_g:6.2f}  "
        f"({n_u}/{n_g} in-radius faces, {len(u_faces)}/{len(g_faces)} total)  gate: {gate}"
    )
    return dev_g


def main() -> int:
    print("Flow-guide exit gate: mean in-radius deviation in degrees, folded mod 90.")
    print("Random field = 22.5; proposal target <= 15.\n")
    measured = []
    for radius_fraction in (0.03, 0.06, 0.12):
        print(f"influence radius = {radius_fraction:.2f} x bbox diagonal")
        for name, axis, other in (("spot.obj", 0, 2), ("stanford-bunny.obj", 1, 2)):
            value = run_model(name, 2000, axis, other, radius_fraction)
            if value is not None:
                measured.append(value)
        print()
    if measured:
        worst = max(measured)
        mean = sum(measured) / len(measured)
        print(f"mean guided deviation {mean:.2f} deg, worst {worst:.2f} deg over "
              f"{len(measured)} runs")
        print("EXIT GATE (<= 15 deg on every run):", "MET" if worst <= 15.0 else "NOT MET")
    return 0


if __name__ == "__main__":
    sys.exit(main())
