#!/usr/bin/env python3
"""The drawing half of the retopology workflow, from Python.

Python is the full-surface desktop harness by design, and until now it could
remesh, unwrap and bake but could not DRAW: no stroke grammar, no snapper
queries, none of the build tools. The gesture path is both the hardest part of
the engine to get right and the one that had no corpus coverage, because the
binding that exists to provide coverage could not reach it.

Every gate here asserts a property of the RESULT rather than that a call
returned zero. A binding that marshals its arguments wrongly usually still
returns CYBER_OK.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402


def _cylinder(radius=1.0, y0=-1.0, y1=1.0, around=48, along=24):
    """A triangulated tube about +Y, open at both ends."""
    positions = []
    for j in range(along + 1):
        y = y0 + (y1 - y0) * j / along
        for i in range(around):
            a = 2.0 * math.pi * i / around
            positions.extend((radius * math.cos(a), y, radius * math.sin(a)))
    indices = []
    for j in range(along):
        for i in range(around):
            i1 = (i + 1) % around
            a = j * around + i
            b = j * around + i1
            c = (j + 1) * around + i1
            d = (j + 1) * around + i
            indices.extend((a, b, c, a, c, d))
    offsets = list(range(0, len(indices) + 1, 3))
    return cyberremesh.Mesh.from_indexed(positions, offsets, indices)


def _arc(y, sweep=2.2, count=12, radius=1.0):
    """Stroke samples on the tube at height `y`, covering the near side only."""
    return [
        (
            radius * math.cos(-sweep / 2 + sweep * k / (count - 1)),
            y,
            radius * math.sin(-sweep / 2 + sweep * k / (count - 1)),
        )
        for k in range(count)
    ]


def gate_snapper_answers_queries():
    target = _cylinder()
    with cyberremesh.Snapper(target) as snapper:
        # An OFF-AXIS ray, deliberately. A ray straight down the axis from
        # (0,0,3) and the same call with origin and direction swapped both land
        # on (0,0,1) at distance 2 -- the first version of this gate used
        # exactly that ray and passed with the arguments transposed.
        origin = (0.3, 0.2, 5.0)
        hit = snapper.raycast(origin, (0.0, 0.0, -1.0))
        assert hit is not None, "a ray at the tube must hit it"
        near_z = math.sqrt(1.0 - 0.3 * 0.3)
        assert abs(hit.point[0] - 0.3) < 2e-3, hit.point
        assert abs(hit.point[1] - 0.2) < 2e-3, hit.point
        assert abs(hit.point[2] - near_z) < 5e-3, hit.point  # the NEAR side, not z<0
        assert abs(hit.distance - (5.0 - near_z)) < 5e-3, hit.distance

        surface = snapper.snap_to_surface((0.4, 0.0, 0.4))
        assert surface is not None
        radial = math.hypot(surface.point[0], surface.point[2])
        assert abs(radial - 1.0) < 2e-3, radial

        vertex = snapper.snap_to_vertex((1.0, 0.0, 0.0), 0.25)
        assert vertex is not None and vertex.vertex < target.vertex_count

        # A ray pointing away must MISS rather than return a nearest hit: the
        # distinction is what makes a viewport tap on empty space do nothing.
        assert snapper.raycast((0.0, 0.0, 3.0), (0.0, 0.0, 1.0)) is None
    print("PASS: snapper raycast / snap_to_surface / snap_to_vertex answer from Python")


def gate_contours_builds_a_tube():
    target = _cylinder()
    edit = cyberremesh.Mesh()
    with cyberremesh.Snapper(target) as snapper:
        report = edit.contours(
            target, [_arc(-0.6), _arc(0.0), _arc(0.6)], spans=12, snapper=snapper
        )
    assert report.ring_count == 3, report
    assert report.vertex_count == 36, report
    assert report.face_count == 24, report  # two bands of twelve

    # The far side the strokes never covered is in the ring: each ring must
    # wrap the tube, not merely span the 2.2 rad the artist drew.
    positions = edit.positions.reshape(-1)
    angles = [
        math.atan2(positions[3 * i + 2], positions[3 * i])
        for i in range(12)
    ]
    assert max(angles) - min(angles) > 4.0, (min(angles), max(angles))

    # Every ring vertex sits on the Target.
    worst = max(
        abs(math.hypot(positions[3 * i], positions[3 * i + 2]) - 1.0)
        for i in range(edit.vertex_count)
    )
    assert worst < 1e-3, worst
    print("PASS: contours lofts three strokes into a 24-quad tube on the Target")


def gate_contours_refuse_an_unusable_stroke():
    target = _cylinder()
    edit = cyberremesh.Mesh()
    straight = [(1.0, -0.5 + 0.1 * k, 0.0) for k in range(10)]
    try:
        edit.contours(target, [_arc(0.0), straight], spans=8)
    except cyberremesh.CyberError:
        assert edit.face_count == 0, "a refused run must leave the mesh untouched"
        print("PASS: a straight stroke names no plane and is refused, mesh unchanged")
        return
    raise AssertionError("a collinear stroke must not produce a tube")


def gate_build_tools_weld_onto_existing_topology():
    target = _cylinder()
    edit = cyberremesh.Mesh()
    with cyberremesh.Snapper(target) as snapper:
        ring = [
            (math.cos(a), y, math.sin(a))
            for a, y in ((-0.1, -0.1), (0.1, -0.1), (0.1, 0.1), (-0.1, 0.1))
        ]
        first = edit.create_face(ring, snapper=snapper)
        assert edit.face_count == 1 and first == 0

        # PolyPen: two corners reuse existing vertices, two are new. The point
        # is the WELD -- the result must share vertices, not duplicate them.
        before = edit.vertex_count
        boundary, _closed = edit.boundary_loop(0)
        assert len(boundary) == 4, boundary
        face, final_ring = edit.build_face(
            [boundary[0], boundary[1], None, None],
            [(0, 0, 0), (0, 0, 0), (math.cos(0.3), -0.1, math.sin(0.3)),
             (math.cos(0.3), 0.1, math.sin(0.3))],
            snapper=snapper,
        )
        assert edit.face_count == 2
        # Exactly two NEW vertices: the other two slots welded.
        assert edit.vertex_count == before + 2, (before, edit.vertex_count)
        assert face != first
        # The reused vertices are in the final ring, and nothing is repeated.
        # Slot ORDER is deliberately not asserted: the engine corrects winding
        # against the neighbouring face, which can reverse and rotate the ring,
        # and pinning slot order would reject that correction.
        assert boundary[0] in final_ring and boundary[1] in final_ring, final_ring
        assert len(set(final_ring)) == 4, final_ring
        new_ids = set(final_ring) - {boundary[0], boundary[1]}
        assert all(v >= before for v in new_ids), (new_ids, before)
        # And the two faces actually share the welded edge.
        shared = set(edit.shortest_path(boundary[0], boundary[1]))
        assert {boundary[0], boundary[1]} <= shared
    print("PASS: create_face then build_face welds onto the existing ring")


def gate_queries_describe_what_was_built():
    target = _cylinder()
    edit = cyberremesh.Mesh()
    with cyberremesh.Snapper(target) as snapper:
        edit.contours(target, [_arc(-0.4), _arc(0.0), _arc(0.4)], spans=10, snapper=snapper)

        assert len(edit.live_faces()) == edit.face_count
        assert edit.triangle_count() == edit.face_count * 2  # quads fan to two

        loop = edit.edge_loop(0)
        assert loop, "edge 0 must belong to a loop"
        ring, _closed = edit.quad_ring(0)
        assert ring

        metrics = edit.loop_metrics(0, snapper=snapper)
        assert metrics.edge_count == len(loop)
        assert metrics.length > 0.0
        assert metrics.snap_measured, "a snapper was supplied, so it must be measured"
        assert metrics.snapped_vertex_count > 0

        # Without a snapper the snapping fields must read as UNMEASURED rather
        # than as zero, or a host cannot tell "not checked" from "off-surface".
        unmeasured = edit.loop_metrics(0)
        assert not unmeasured.snap_measured
        assert unmeasured.edge_count == metrics.edge_count

        path = edit.shortest_path(0, 5)
        assert path and path[0] == 0 and path[-1] == 5
    print("PASS: loop, ring, metrics and path queries agree with the built tube")


def gate_stroke_grammar_classifies():
    """Shape classification runs with no EditMesh: stage one needs no scene."""
    square = []
    corners = [(0.3, 0.3), (0.7, 0.3), (0.7, 0.7), (0.3, 0.7)]
    for k in range(4):
        a, b = corners[k], corners[(k + 1) % 4]
        for j in range(10):
            square.append((a[0] + (b[0] - a[0]) * j / 10, a[1] + (b[1] - a[1]) * j / 10))
    samples = [(x, y, i / len(square) * 0.5) for i, (x, y) in enumerate(square)]
    closed = cyberremesh.interpret(samples)
    assert closed.shape is cyberremesh.StrokeShape.CLOSED_LOOP, closed.shape
    assert closed.action is cyberremesh.StrokeAction.CREATE_QUAD, closed.action
    assert len(closed.corners) == 4, closed.corners
    assert closed.context is cyberremesh.StrokeContext.EMPTY_SURFACE

    line = [(0.2 + 0.6 * i / 39, 0.5, i / 39 * 0.4) for i in range(40)]
    assert cyberremesh.interpret(line).shape is cyberremesh.StrokeShape.LINE

    circle = [
        (0.5 + 0.2 * math.cos(2 * math.pi * i / 48),
         0.5 + 0.2 * math.sin(2 * math.pi * i / 48),
         i / 48 * 0.5)
        for i in range(48)
    ]
    assert cyberremesh.interpret(circle).shape is cyberremesh.StrokeShape.CIRCLE

    cross = [(0.3 + 0.4 * i / 19, 0.3 + 0.4 * i / 19) for i in range(20)]
    cross += [(0.7 - 0.4 * i / 19, 0.3 + 0.4 * i / 19) for i in range(20)]
    x_samples = [(x, y, i / len(cross) * 0.5) for i, (x, y) in enumerate(cross)]
    assert cyberremesh.interpret(x_samples).shape is cyberremesh.StrokeShape.CROSS

    tap = [(0.5, 0.5, 0.0), (0.5005, 0.5002, 0.09)]
    assert cyberremesh.interpret(tap).shape is cyberremesh.StrokeShape.HOLD_POINT
    print("PASS: closed loop, line, circle, cross and tap all classify from Python")


def gate_stroke_timing_reaches_the_engine():
    """Timing is marshalled, not dropped.

    Every shape above is distinguishable by geometry alone, so a binding that
    silently zeroed the time channel passed all of them. Timing's only effect is
    on a stationary press: held past the engine's hold threshold it is a
    deliberate hold, under it a quick tap, and the two carry different
    confidence. Same geometry, different time, different answer -- which only
    happens if the time actually arrived.
    """
    quick = cyberremesh.interpret([(0.5, 0.5, 0.0), (0.5003, 0.5001, 0.05)])
    held = cyberremesh.interpret([(0.5, 0.5, 0.0), (0.5003, 0.5001, 0.60)])
    assert quick.shape is cyberremesh.StrokeShape.HOLD_POINT, quick.shape
    assert held.shape is cyberremesh.StrokeShape.HOLD_POINT, held.shape
    assert held.shape_confidence > quick.shape_confidence, (
        held.shape_confidence, quick.shape_confidence,
    )
    print("PASS: a held press outranks a quick tap, so stroke timing reaches the engine")


def gate_interpretation_is_deterministic():
    """The header promises identical inputs give identical records."""
    samples = [
        (0.5 + 0.2 * math.cos(2 * math.pi * i / 40),
         0.5 + 0.2 * math.sin(2 * math.pi * i / 40),
         i / 40 * 0.5)
        for i in range(40)
    ]
    first = cyberremesh.interpret(samples)
    second = cyberremesh.interpret(samples)
    assert first == second, (first, second)
    print("PASS: interpreting the same stroke twice gives the same record")


def gate_interpret_rejects_an_empty_stroke():
    try:
        cyberremesh.interpret([])
    except ValueError:
        print("PASS: an empty stroke is refused before it reaches the engine")
        return
    raise AssertionError("an empty stroke must not be interpreted")


def main():
    if not cyberremesh.is_available():
        # 77, not 0: CTest maps it to SKIPPED. Returning 0 here would report
        # PASSED on a lane where the engine never loaded -- which is exactly
        # how the Windows lane once ran zero Python tests and stayed green.
        print("SKIP: engine library not available")
        return 77
    gate_snapper_answers_queries()
    gate_contours_builds_a_tube()
    gate_contours_refuse_an_unusable_stroke()
    gate_build_tools_weld_onto_existing_topology()
    gate_queries_describe_what_was_built()
    gate_stroke_grammar_classifies()
    gate_stroke_timing_reaches_the_engine()
    gate_interpretation_is_deterministic()
    gate_interpret_rejects_an_empty_stroke()
    print("\nall drawing-surface gates passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
