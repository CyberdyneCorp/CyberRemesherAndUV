#!/usr/bin/env python3
"""Binding-parity gates for the ZRemesher track.

The engine-bindings spec asks for more than "the method is reachable": the C
ABI and Python must expose the ZRemesher PARAMETERS (quality mode, symmetry,
guide mode) and the run REPORT (layout statistics, quality score), and parity
must hold — anything the headless CLI can do must be reachable from Python.

This asserts both of the spec's scenarios, the rejections that keep a mistyped
mode from being silently reinterpreted, and the CLI/Python parity itself.

    python python/cyberremesh/tests/test_zremesher_bindings.py

Skips with 77 (CTest SKIP) when the shared library is not loadable. The CLI
parity check additionally skips itself when no built ``cyberremesh`` binary is
found, so the rest of the file still runs from a library-only build.

Deliberately procedural fixtures: examples/models/ is git-ignored and fetched
on demand, so a gate needing the corpus would be a gate that never runs in CI.
"""

import math
import os
import subprocess
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

_REPO = os.path.dirname(  # <repo>/python/cyberremesh/tests -> <repo>
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
_TIMEOUT = 1800

import cyberremesh
from cyberremesh import FlowGuide, Mesh, RemeshParams, ZRemesherParams, remesh


def _sphere_obj(path, rings=24, segments=36, radius=1.0):
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
    return path


def _torus_obj(path, major=1.0, minor=0.35, u=64, v=32):
    """A torus, for anything that has to be sensitive to ADAPTIVITY.

    A sphere is useless here: its curvature is constant, so adaptivity 0 and
    adaptivity 1 size it identically and a test built on one cannot see the
    difference — it passes whether or not the parameter is forwarded. A torus
    varies strongly between its inner and outer equator, so the setting shows.
    """
    verts, faces = [], []
    for i in range(u):
        a = 2.0 * math.pi * i / u
        for j in range(v):
            b = 2.0 * math.pi * j / v
            verts.append(((major + minor * math.cos(b)) * math.cos(a),
                          minor * math.sin(b),
                          (major + minor * math.cos(b)) * math.sin(a)))

    def at(i, j):
        return (i % u) * v + (j % v)

    for i in range(u):
        for j in range(v):
            faces.append([at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)])
    with open(path, "w") as fh:
        for p in verts:
            fh.write("v {0:.6f} {1:.6f} {2:.6f}\n".format(*p))
        for f in faces:
            fh.write("f " + " ".join(str(k + 1) for k in f) + "\n")
    return path


def _find_cli():
    """The most recently built ``cyberremesh``, or None.

    Newest wins deliberately: a build tree usually holds several configurations,
    and picking whichever one os.walk happens to reach first silently tests a
    stale binary — which reports a missing feature as a failure of the feature.
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


def gate_python_drives_zremesher(work):
    """Spec scenario: Python requests the method with a quality mode and a
    symmetry axis, and the report carries the layout statistics and score."""
    mesh = Mesh.load_obj(_sphere_obj(os.path.join(work, "sphere.obj")))
    params = RemeshParams(target_quad_count=800, quad_method="zremesher")

    out = remesh(mesh, params, zremesher=ZRemesherParams(quality="best", symmetry="x"))
    report = out.zremesher_report
    assert report is not None, "the zremesher path attached no run report"

    # Layout statistics: the whole point is that these are readable WITHOUT
    # scraping stderr.
    assert report.layouts > 0, "no topology layout was traced"
    assert report.layouts == report.layouts_valid, (
        "{0} of {1} layouts failed their own invariants".format(
            report.layouts - report.layouts_valid, report.layouts))
    assert report.nodes > 0 and report.arcs > 0 and report.patches > 0, (
        "layout statistics came back empty: {0}".format(report))

    # Quality score: `best` must NAME the candidate it kept.
    assert report.selected_candidate in ("multires", "single-level"), (
        "quality='best' selected {0!r}".format(report.selected_candidate))
    assert report.quality_score > 0.0, "selected a candidate with no score"

    # Symmetry, checked on the RESULT by the engine rather than assumed.
    assert report.symmetry_applied, "symmetry='x' did not apply"
    assert report.topologically_symmetric, (
        "symmetry='x' produced a mesh that is not topologically symmetric")
    assert report.mirrored_faces > 0, "nothing was mirrored"
    print("PASS: python drove zremesher — {0} layout(s), candidate {1!r} "
          "(score {2:.3f}), symmetry exact".format(
              report.layouts, report.selected_candidate, report.quality_score))


def gate_topology_guides_from_python(work):
    """Spec scenario: a CLOSED guide in topology mode is forwarded as such.

    Forwarding is what is asserted, and it is asserted through an OUTPUT
    difference rather than by trusting the struct: an orientation guide and a
    topology guide over the same stroke must not produce the same mesh, or the
    mode never reached the engine. Adherence itself is measured by the
    release-gate suite, which owns that number.
    """
    mesh = Mesh.load_obj(_sphere_obj(os.path.join(work, "guides.obj")))
    params = RemeshParams(target_quad_count=800, quad_method="zremesher")

    # A tilted ring: nothing about a sphere wants a loop there, so a mode that
    # is actually honored has to change the result.
    ring = []
    for k in range(48):
        a = 2.0 * math.pi * k / 48
        x, z = math.cos(a), math.sin(a)
        y = 0.35 * math.cos(a)
        n = math.sqrt(x * x + y * y + z * z)
        ring.append([x / n, y / n, z / n])

    orientation = remesh(mesh, params, guides=[
        FlowGuide(points=ring, strength=1.0, radius=0.25, mode="orientation", closed=True)])
    topology = remesh(mesh, params, guides=[
        FlowGuide(points=ring, strength=1.0, radius=0.25, mode="topology", closed=True)])

    same_counts = (orientation.stats.vertices == topology.stats.vertices
                   and orientation.stats.quads == topology.stats.quads)
    assert not same_counts, (
        "orientation and topology guides produced identical counts "
        "({0} verts / {1} quads) — the guide mode did not reach the engine".format(
            orientation.stats.vertices, orientation.stats.quads))
    print("PASS: guide mode forwarded — orientation {0}v/{1}q vs topology {2}v/{3}q".format(
        orientation.stats.vertices, orientation.stats.quads,
        topology.stats.vertices, topology.stats.quads))


def gate_bad_values_are_rejected(work):
    """Rejected, never reinterpreted.

    A symmetry axis quietly clamped to "none" hands back an asymmetric mesh for
    a symmetry request, and a typo'd guide mode quietly biases the field instead
    of cutting a loop. Both are worse than an error.
    """
    mesh = Mesh.load_obj(_sphere_obj(os.path.join(work, "reject.obj"), rings=8, segments=12))
    zr_params = RemeshParams(target_quad_count=200, quad_method="zremesher")

    cases = [
        ("quality", dict(zremesher=ZRemesherParams(quality="turbo")), zr_params),
        ("symmetry", dict(zremesher=ZRemesherParams(symmetry="w")), zr_params),
        ("guide mode", dict(guides=[FlowGuide(points=[[0, 1, 0], [1, 0, 0]],
                                              radius=0.3, mode="topolgy")]), zr_params),
        # zremesher=... on another method is a mismatch, not a hint to switch.
        ("method mismatch", dict(zremesher=ZRemesherParams()),
         RemeshParams(target_quad_count=200, quad_method="quad-cover")),
    ]
    for name, kwargs, params in cases:
        try:
            remesh(mesh, params, **kwargs)
        except ValueError:
            continue
        raise AssertionError("a bad {0} was accepted instead of rejected".format(name))
    print("PASS: {0} bad values rejected, none reinterpreted".format(len(cases)))


def gate_cli_and_python_agree(work):
    """Parity, measured: the same request through both surfaces, same mesh.

    This is the regression test for a real divergence. cyber_remesh with
    CYBER_QUAD_ZREMESHER never forwarded `adaptivity` — it left the option at
    its 0.0 default while the CLI has always passed the parameter through — so
    with the default adaptivity of 1.0 the SAME request gave a different mesh
    from Python than from the CLI.
    """
    cli = _find_cli()
    if cli is None:
        print("SKIP: no built cyberremesh CLI to compare against")
        return

    # A torus, not a sphere: the divergence is in ADAPTIVITY, and a
    # constant-curvature surface is sized identically at 0 and at 1 — a sphere
    # fixture passes whether or not the parameter is forwarded. Verified by
    # mutation: with the forwarding removed the gate fails on a torus (CLI
    # 696v vs Python 698v) and still passes on a sphere, which is why the
    # fixture here is a torus.
    src = _torus_obj(os.path.join(work, "parity.obj"))
    out = os.path.join(work, "parity_cli.obj")
    proc = subprocess.run(
        [cli, "--input", src, "--output", out, "--quad-method", "zremesher",
         "--target-quads", "800", "--quiet"],
        capture_output=True, text=True, timeout=_TIMEOUT)
    assert proc.returncode == 0, proc.stderr[-2000:]
    cli_verts, cli_faces = _read_obj(out)

    py = remesh(Mesh.load_obj(src), RemeshParams(target_quad_count=800, quad_method="zremesher"))
    assert py.stats.vertices == len(cli_verts) and py.stats.quads + py.stats.triangles + \
        py.stats.other_polygons == len(cli_faces), (
        "CLI and Python disagree for the same request: CLI {0}v/{1}f, "
        "Python {2}v/{3}f".format(len(cli_verts), len(cli_faces),
                                  py.stats.vertices,
                                  py.stats.quads + py.stats.triangles + py.stats.other_polygons))
    print("PASS: CLI and Python agree — {0} vertices, {1} faces".format(
        len(cli_verts), len(cli_faces)))


def main():
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the capi module)")
        return 77
    with tempfile.TemporaryDirectory() as work:
        gate_python_drives_zremesher(work)
        gate_topology_guides_from_python(work)
        gate_bad_values_are_rejected(work)
        gate_cli_and_python_agree(work)
    print("all ZRemesher binding gates passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
