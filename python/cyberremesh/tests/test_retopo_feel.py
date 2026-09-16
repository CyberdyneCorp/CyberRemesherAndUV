#!/usr/bin/env python3
"""Region relax, loop slide and interactive symmetry, from Python.

Every gate asserts a property of the RESULT, with fixtures chosen so a binding
that drops or swaps a field produces a different answer: a flipped
``working_side_positive`` must mirror the OTHER half, a negated ``t`` must slide
the other way, and a region one ring too small must leave a two-hop vertex
exactly where it was.

Numpy-free on purpose (reads through ``vertex_position``): this package runs
without numpy, and so does one of the CI lanes that runs this file.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402


def _grid(nx, ny, x0=0.0):
    """(nx+1) x (ny+1) quad grid in the XY plane, starting at x = x0."""
    positions = []
    for j in range(ny + 1):
        for i in range(nx + 1):
            positions.extend((x0 + float(i), float(j), 0.0))
    at = lambda i, j: j * (nx + 1) + i  # noqa: E731
    indices = []
    for j in range(ny):
        for i in range(nx):
            indices.extend((at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)))
    offsets = list(range(0, len(indices) + 1, 4))
    return cyberremesh.Mesh.from_indexed(positions, offsets, indices), at


def _move(mesh, vertex, position):
    """Set one vertex's position without numpy."""
    flat = []
    for v in range(mesh.vertex_count):
        p = position if v == vertex else mesh.vertex_position(v)
        flat.extend(p)
    mesh.set_positions(flat)


def _edge(mesh, a, b):
    """The edge joining vertices a and b, found through the picking query."""
    pa, pb = mesh.vertex_position(a), mesh.vertex_position(b)
    mid = tuple((pa[k] + pb[k]) / 2 for k in range(3))
    edge = mesh.nearest_edge(mid, 0.1)
    assert edge is not None, (a, b)
    return edge


def gate_slide_moves_the_whole_loop_to_one_side():
    mesh, at = _grid(6, 6)
    report = mesh.slide_loop(_edge(mesh, at(2, 3), at(3, 3)), 0.25)
    assert report.loop_vertices == 7 and report.moved == 7, report
    ys = {round(mesh.vertex_position(at(i, 3))[1], 5) for i in range(7)}
    assert len(ys) == 1, ys  # one value: every vertex went the same way
    (y,) = ys
    assert abs(abs(y - 3.0) - 0.25) < 1e-5, y
    # Nothing off the loop moved.
    assert mesh.vertex_position(at(0, 2)) == (0.0, 2.0, 0.0)
    print("PASS: a loop slide moves every loop vertex a quarter-rail to the same side")


def gate_slide_sign_selects_the_side():
    up, at = _grid(5, 5)
    down, _ = _grid(5, 5)
    up.slide_loop(_edge(up, at(1, 2), at(2, 2)), 0.4)
    down.slide_loop(_edge(down, at(1, 2), at(2, 2)), -0.4)
    dy_up = up.vertex_position(at(0, 2))[1] - 2.0
    dy_down = down.vertex_position(at(0, 2))[1] - 2.0
    assert abs(abs(dy_up) - 0.4) < 1e-5, dy_up
    assert abs(dy_down + dy_up) < 1e-5, (dy_up, dy_down)  # mirror images
    print("PASS: t and -t slide to opposite sides by the same distance")


def gate_slide_refuses_a_full_rail():
    mesh, at = _grid(4, 4)
    edge = _edge(mesh, at(1, 2), at(2, 2))
    before = [mesh.vertex_position(v) for v in range(mesh.vertex_count)]
    for bad in (1.0, -1.0, float("nan")):
        try:
            mesh.slide_loop(edge, bad)
        except cyberremesh.CyberError:
            continue
        raise AssertionError(f"slide_loop accepted t={bad!r}")
    after = [mesh.vertex_position(v) for v in range(mesh.vertex_count)]
    assert before == after
    print("PASS: |t| >= 1 and NaN are refused with the mesh unchanged")


def gate_region_relax_reaches_exactly_its_rings():
    """Disturb a vertex two hops from the seed: rings=2 moves it, rings=1 must not."""

    def run(rings):
        mesh, at = _grid(10, 10)
        seed, two_hops = at(5, 5), at(7, 5)
        _move(mesh, two_hops, (7.35, 5.3, 0.0))
        disturbed = mesh.vertex_position(two_hops)
        far_before = mesh.vertex_position(at(0, 0))
        report = mesh.relax_region([seed], rings=rings, iterations=4, auto_pin_corners=False)
        return mesh.vertex_position(two_hops), disturbed, report, mesh.vertex_position(at(0, 0)), far_before

    inside, disturbed, report, far, far_before = run(2)
    assert inside != disturbed, "a vertex two hops out must move when rings=2"
    assert math.hypot(inside[0] - 7, inside[1] - 5) < math.hypot(disturbed[0] - 7, disturbed[1] - 5)
    assert report.moved > 0
    assert far == far_before  # outside the region: exact

    outside, disturbed, _report, _far, _fb = run(1)
    assert outside == disturbed, "one ring short must leave it exactly in place"
    print("PASS: region relax reaches rings=2 and stops exactly at rings=1")


def gate_region_relax_honours_pins():
    mesh, at = _grid(6, 6)
    _move(mesh, at(3, 3), (3.3, 3.3, 0.0))
    _move(mesh, at(3, 4), (3.2, 4.2, 0.0))
    pinned_at = mesh.vertex_position(at(3, 4))
    mesh.relax_region([at(3, 3)], rings=2, iterations=4, auto_pin_corners=False, pinned=[at(3, 4)])
    assert mesh.vertex_position(at(3, 4)) == pinned_at
    print("PASS: a pinned vertex inside the region does not move")


def gate_region_relax_rejects_bad_parameters():
    mesh, at = _grid(3, 3)
    for kwargs in ({"rings": -1}, {"iterations": 0}, {"strength": float("nan")}, {"strength": 1.5}):
        try:
            mesh.relax_region([at(1, 1)], **kwargs)
        except cyberremesh.CyberError:
            continue
        raise AssertionError(f"relax_region accepted {kwargs}")
    print("PASS: negative rings, zero iterations and out-of-range strength are refused")


def gate_apply_symmetry_mirrors_the_working_half():
    """A half-grid on x in [0, 3] mirrored across x = 0.

    The flag is checked from both sides: with working_side_positive=True the
    +x half is authored and gains a twin; with False the mesh has nothing on
    the -x side to mirror, so nothing is added. A binding that dropped the flag
    would give the same answer both times.
    """
    positive, at = _grid(3, 2)
    added = positive.apply_symmetry(
        cyberremesh.Symmetry(normal=(1.0, 0.0, 0.0), weld_tolerance=1e-3, working_side_positive=True)
    )
    assert added == 6, added  # every face of the half gets a twin
    assert positive.face_count == 12
    xs = sorted({round(positive.vertex_position(v)[0], 5) for v in range(positive.vertex_count)})
    assert xs == [-3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0], xs
    # The x = 0 column is SHARED, not duplicated: 7 columns x 3 rows.
    assert positive.vertex_count == 21, positive.vertex_count

    negative, _ = _grid(3, 2)
    added = negative.apply_symmetry(
        cyberremesh.Symmetry(normal=(1.0, 0.0, 0.0), weld_tolerance=1e-3, working_side_positive=False)
    )
    assert added == 0 and negative.face_count == 6, (added, negative.face_count)
    print("PASS: apply_symmetry mirrors the working half, shares the seam, honours the side flag")


def gate_snap_and_resymmetrize():
    mesh, at = _grid(2, 2, x0=-1.0)  # columns at x = -1, 0, 1
    plane = cyberremesh.Symmetry(normal=(1.0, 0.0, 0.0), weld_tolerance=0.05)

    _move(mesh, at(1, 1), (0.03, 1.0, 0.0))  # near the plane, within tolerance
    assert mesh.snap_symmetry_plane(plane) >= 1
    assert mesh.vertex_position(at(1, 1))[0] == 0.0  # exactly on it

    # Perturb the OFF side (x < 0); re-symmetrize must copy the working side
    # back over it, and must not touch the working side.
    _move(mesh, at(0, 2), (-1.2, 2.15, 0.0))
    working_before = mesh.vertex_position(at(2, 2))
    report = mesh.resymmetrize(plane, match_tolerance=0.5)
    assert report.matched >= 1, report
    x, y, _z = mesh.vertex_position(at(0, 2))
    assert abs(x + 1.0) < 1e-5 and abs(y - 2.0) < 1e-5, (x, y)
    assert mesh.vertex_position(at(2, 2)) == working_before
    print("PASS: snap_symmetry_plane lands exactly on the plane; resymmetrize repairs the off side only")


def main():
    if not cyberremesh.is_available():
        print("SKIP: engine library not available")
        return 77
    gate_slide_moves_the_whole_loop_to_one_side()
    gate_slide_sign_selects_the_side()
    gate_slide_refuses_a_full_rail()
    gate_region_relax_reaches_exactly_its_rings()
    gate_region_relax_honours_pins()
    gate_region_relax_rejects_bad_parameters()
    gate_apply_symmetry_mirrors_the_working_half()
    gate_snap_and_resymmetrize()
    print("\nall retopology-feel gates passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
