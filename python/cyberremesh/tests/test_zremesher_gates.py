#!/usr/bin/env python3
"""Release gates for the ZRemesher-class retopology track.

The examples REPORT what they measure; this ASSERTS it. Everything here is a
property the work established and that a later change must not silently undo:

  1. the topology layout validates on every model it traces
  2. `--symmetry x` produces exactly mirrored connectivity
  3. a topology guide is followed markedly better than an orientation guide
  4. `--quality best` picks a candidate and says which

Deliberately procedural: examples/models/ is git-ignored and downloaded on
demand, so a gate that needed the corpus would be a gate that never ran in CI.

    python python/cyberremesh/tests/test_zremesher_gates.py

Skips with 77 (CTest SKIP) when the CLI or NumPy/SciPy are missing.
"""

import json
import math
import os
import subprocess
import sys
import tempfile

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_TIMEOUT = 1800


def _find_cli():
    """The most recently built ``cyberremesh``, or None.

    Newest wins deliberately. A build tree usually holds several configurations
    (cpu-headless, cpu-headless-debug, ...) and taking whichever one os.walk
    reached first tested whatever was there — a stale binary then reports a
    missing feature as a failure of the feature, which is exactly how these
    gates read as red on an up-to-date checkout.
    """
    build = os.path.join(_REPO, "build")
    if not os.path.isdir(build):
        return None
    found = []
    for root, _dirs, files in os.walk(build):
        candidate = os.path.join(root, "cyberremesh")
        if "cyberremesh" in files and os.access(candidate, os.X_OK):
            found.append(candidate)
    if not found:
        return None
    return max(found, key=os.path.getmtime)


def _have_numpy():
    try:
        import numpy  # noqa: F401
        from scipy.spatial import cKDTree  # noqa: F401
    except ImportError:
        return False
    return True


def _sphere_obj(path, rings=32, segments=48, radius=1.0):
    verts, faces = [], []
    for i in range(rings + 1):
        theta = math.pi * i / rings
        for j in range(segments):
            phi = 2.0 * math.pi * j / segments
            verts.append((radius * math.sin(theta) * math.cos(phi),
                          radius * math.cos(theta),
                          radius * math.sin(theta) * math.sin(phi)))
    def at(i, j):
        return i * segments + (j % segments)
    for i in range(rings):
        for j in range(segments):
            faces.append([at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)])
    with open(path, "w") as fh:
        for v in verts:
            fh.write("v {0:.6f} {1:.6f} {2:.6f}\n".format(*v))
        for f in faces:
            fh.write("f " + " ".join(str(i + 1) for i in f) + "\n")


def _read_obj(path):
    verts, faces = [], []
    with open(path) as fh:
        for line in fh:
            if line.startswith("v "):
                p = line.split()
                verts.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                faces.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return verts, faces


def _run(cli, args, env=None):
    full = dict(os.environ)
    full.update(env or {})
    return subprocess.run([cli] + args, capture_output=True, text=True, timeout=_TIMEOUT,
                          env=full)


def gate_layout_validates(cli, work):
    """Every layout the tracer produces must satisfy its own invariants."""
    src = os.path.join(work, "sphere.obj")
    _sphere_obj(src)
    proc = _run(cli, ["--input", src, "--output", os.path.join(work, "l.obj"),
                      "--quad-method", "zremesher", "--target-quads", "1500"],
                {"CYBER_ZR_LAYOUT": "1"})
    assert proc.returncode == 0, proc.stderr[-2000:]
    reports = [ln for ln in proc.stderr.splitlines() if "[zr] layout:" in ln]
    assert reports, "the zremesher method traced no layout at all:\n" + proc.stderr[-2000:]
    for line in reports:
        assert "valid=1" in line, "layout failed its invariants: " + line
    print("PASS: {0} layout(s) traced, all valid".format(len(reports)))


def gate_symmetry_is_exact(cli, work):
    """--symmetry must mirror CONNECTIVITY, not merely shape."""
    import numpy as np
    from scipy.spatial import cKDTree

    src = os.path.join(work, "sym_src.obj")
    _sphere_obj(src)
    out = os.path.join(work, "sym.obj")
    proc = _run(cli, ["--input", src, "--output", out, "--quad-method", "zremesher",
                      "--symmetry", "x", "--target-quads", "1500", "--quiet"])
    assert proc.returncode == 0, proc.stderr[-2000:]

    verts, faces = _read_obj(out)
    assert faces, "symmetric run produced no faces"
    pts = np.asarray(verts, dtype=float)
    plane_x = 0.5 * (float(pts[:, 0].min()) + float(pts[:, 0].max()))
    edges = {(min(f[i], f[(i + 1) % len(f)]), max(f[i], f[(i + 1) % len(f)]))
             for f in faces for i in range(len(f))}
    tol = 0.25 * float(np.mean([np.linalg.norm(pts[a] - pts[b]) for a, b in edges]))

    # Nearest-within-tolerance, NOT a quantized key lookup: rounding to a grid
    # and comparing keys collides distinct vertices and misses partners just
    # across a cell boundary, and reports a perfectly symmetric mesh as broken.
    tree = cKDTree(pts)
    mirrored = pts.copy()
    mirrored[:, 0] = 2.0 * plane_x - mirrored[:, 0]
    distance, partner = tree.query(mirrored, distance_upper_bound=tol)
    matched = distance <= tol
    unmatched_v = int((~matched).sum())
    assert unmatched_v == 0, "{0} vertices have no mirror partner".format(unmatched_v)

    face_sets = {tuple(sorted(f)) for f in faces}
    unmatched_f = sum(
        1 for f in faces
        if tuple(sorted(int(partner[i]) for i in f)) not in face_sets)
    assert unmatched_f == 0, "{0} faces have no mirror image".format(unmatched_f)
    print("PASS: symmetry exact — 0 unmatched vertices, 0 unmatched faces "
          "over {0} faces".format(len(faces)))


def gate_topology_guide_beats_orientation(cli, work):
    """A topology guide must be followed markedly better than an orientation one."""
    import numpy as np

    src = os.path.join(work, "guide_src.obj")
    _sphere_obj(src, rings=40, segments=60)
    loop = [[math.cos(t), 0.0, math.sin(t)]
            for t in [i * 2.0 * math.pi / 64 for i in range(64)]]
    dense = np.asarray([[math.cos(t), 0.0, math.sin(t)]
                        for t in [i * 2.0 * math.pi / 256 for i in range(256)]])

    coverage = {}
    for mode in ("orientation", "topology"):
        sidecar = os.path.join(work, "g_{0}.json".format(mode))
        with open(sidecar, "w") as fh:
            json.dump({"version": 1, "guides": [
                {"points": loop, "strength": 1.0, "radius": 0.15,
                 "mode": mode, "closed": True}]}, fh)
        out = os.path.join(work, "g_{0}.obj".format(mode))
        proc = _run(cli, ["--input", src, "--output", out, "--quad-method", "zremesher",
                          "--target-quads", "1200", "--guides", sidecar, "--quiet"])
        assert proc.returncode == 0, proc.stderr[-2000:]
        verts, faces = _read_obj(out)
        pts = np.asarray(verts, dtype=float)
        pairs = set()
        for f in faces:
            for i in range(len(f)):
                x, y = f[i], f[(i + 1) % len(f)]
                pairs.add((min(x, y), max(x, y)))
        edges = np.asarray(sorted(pairs), dtype=int)
        coverage[mode] = _aligned_coverage(pts, edges, dense)

    # Both halves of the measure matter: an edge merely NEAR the guide is not
    # following it, and edges crossing at right angles are everywhere.
    assert coverage["topology"] > coverage["orientation"] + 0.20, (
        "topology guide barely beat orientation: {0:.1%} vs {1:.1%}".format(
            coverage["topology"], coverage["orientation"]))
    print("PASS: guide adherence orientation {0:.1%} -> topology {1:.1%}".format(
        coverage["orientation"], coverage["topology"]))


def _aligned_coverage(pts, edges, samples, max_angle_degrees=45.0):
    import numpy as np

    a = pts[edges[:, 0]]
    b = pts[edges[:, 1]]
    ab = b - a
    len2 = np.einsum("ij,ij->i", ab, ab)
    len2[len2 < 1e-20] = 1e-20
    unit = ab / np.sqrt(len2)[:, None]
    tol = 0.25 * float(np.mean(np.sqrt(len2)))
    cos_limit = math.cos(math.radians(max_angle_degrees))
    tangents = np.roll(samples, -1, axis=0) - samples
    tangents /= np.linalg.norm(tangents, axis=1)[:, None]

    covered = 0
    for i, p in enumerate(samples):
        t = np.clip(np.einsum("ij,ij->i", p - a, ab) / len2, 0.0, 1.0)
        d = np.linalg.norm(p - (a + ab * t[:, None]), axis=1)
        ok = np.abs(unit @ tangents[i]) >= cos_limit
        if np.any(ok) and float(np.min(d[ok])) <= tol:
            covered += 1
    return covered / len(samples)


def gate_quality_best_selects(cli, work):
    """--quality best must run both candidates and name the winner."""
    src = os.path.join(work, "best_src.obj")
    _sphere_obj(src)
    proc = _run(cli, ["--input", src, "--output", os.path.join(work, "b.obj"),
                      "--quad-method", "zremesher", "--quality", "best",
                      "--target-quads", "1200"])
    assert proc.returncode == 0, proc.stderr[-2000:]
    selected = [ln for ln in proc.stderr.splitlines() if "selected candidate:" in ln]
    assert selected, "quality=best named no candidate:\n" + proc.stderr[-2000:]
    print("PASS: " + selected[-1].strip())


def main():
    cli = _find_cli()
    if cli is None:
        print("SKIP: cyberremesh CLI not built")
        return 77
    if not _have_numpy():
        print("SKIP: NumPy/SciPy unavailable")
        return 77
    with tempfile.TemporaryDirectory(prefix="cyber_zr_gates_") as work:
        gate_layout_validates(cli, work)
        gate_symmetry_is_exact(cli, work)
        gate_topology_guide_beats_orientation(cli, work)
        gate_quality_best_selects(cli, work)
    print("all ZRemesher gates passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
