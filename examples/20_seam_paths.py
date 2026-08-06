#!/usr/bin/env python3
"""Auto-routed seam paths — the UV Path tool: click two points, get a seam that
FOLLOWS THE GROOVE.

Hand-placing a UV seam means clicking every edge along a feature. ``SeamPath``
takes waypoints instead and routes the least-cost edge path between them, where
"cost" is edge length scaled by :class:`SeamCostParams`: feature-tagged and
creased edges are cheap, flat surface is expensive. A seam therefore sinks into
a groove instead of taking the shortcut across the flat. Valleys and ridges are
weighted separately (``concave_weight`` / ``convex_weight``) — a seam hides best
in a valley, but a hard convex edge, which is what most CAD parts are made of,
is worth following too.

The figure makes that the whole point. The same router runs twice on the same
two waypoints of a grooved disk:

  * routed   — engine defaults (concave edges at 0.35x cost)
  * shortest — every weight forced to 1.0, i.e. a plain shortest edge path
               (the discrete geodesic), the thing you get without the feature

If those two polylines ever coincide, the cost model is doing nothing. They do
not: the shortest path cuts straight across the flat plate, the routed one wraps
around inside the trench.

The console half also exercises the editing surface a UV tool needs: waypoints
stay live (``move_waypoint_to``), only the touched segments re-route
(``segment_revision``), and ``commit`` / ``revert_commit`` are an undoable pair.
The blue overlay in the top-down panel is that COMMITTED seam, drawn straight
from its edge ids through ``Mesh.edge_endpoints`` + ``Mesh.vertex_position`` —
a committed seam is a set of edge ids and nothing else, so without those two
accessors it could not be rendered at all.

This figure demonstrates the concave term; the convex one behaves the same way
over a ridge. ``SeamCostParams.feature_weight`` is
settable, but nothing in the C ABI can tag an edge as a feature edge, so on a
mesh loaded from disk that term never fires.

    examples/run.sh examples/20_seam_paths.py
"""

import math
import os
import tempfile

import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

import common as c
from cyberremesh import Mesh, SeamCostParams, SeamPath, SeamSet

RING_RADIUS = 0.62      # radius of the circular groove
GROOVE_HALF = 0.13      # half-width of the V cross-section
GROOVE_DEPTH = 0.16     # depth at the bottom of the V
SEGMENTS = 96           # angular resolution


def grooved_disk_obj(path, outer=1.0, segments=SEGMENTS):
    """A flat disk with a circular V-trench cut into it.

    Sampled on a polar grid whose radii land exactly on the trench bottom and
    both shoulders, so the groove floor is a genuine ring of mesh edges with a
    strongly concave dihedral (~80 deg) rather than a staircase approximation.
    The flat plate inside and outside the ring is where a shortcut is tempting.
    """
    r_lo, r_hi = RING_RADIUS - GROOVE_HALF, RING_RADIUS + GROOVE_HALF
    radii = np.unique(np.round(np.concatenate([
        np.linspace(0.0, r_lo, 8),
        np.linspace(r_lo, RING_RADIUS, 5),
        np.linspace(RING_RADIUS, r_hi, 5),
        np.linspace(r_hi, outer, 6),
    ]), 6))

    def height(r):
        return -GROOVE_DEPTH * max(0.0, 1.0 - abs(r - RING_RADIUS) / GROOVE_HALF)

    verts = [(0.0, 0.0, height(0.0))]
    for r in radii[1:]:
        z = height(r)
        for s in range(segments):
            a = 2.0 * math.pi * s / segments
            verts.append((r * math.cos(a), r * math.sin(a), z))

    def idx(ring, s):
        return 1 + ring * segments + (s % segments)

    faces = [[0, idx(0, s), idx(0, s + 1)] for s in range(segments)]
    for ring in range(len(radii) - 2):
        for s in range(segments):
            a, b = idx(ring, s), idx(ring + 1, s)
            cc, d = idx(ring + 1, s + 1), idx(ring, s + 1)
            faces.append([a, b, cc])
            faces.append([a, cc, d])
    c._write_obj(path, verts, faces)


def ring_point(degrees, radius=RING_RADIUS, z=-GROOVE_DEPTH):
    """A point on the groove floor at the given angle."""
    a = math.radians(degrees)
    return np.array([radius * math.cos(a), radius * math.sin(a), z])


def nearest_vertex(positions, target):
    return int(np.argmin(((positions - target) ** 2).sum(axis=1)))


def route(mesh, params, waypoints):
    """Route a path through `waypoints` (vertex ids) and return it. Caller closes."""
    path = SeamPath(mesh, params)
    for v in waypoints:
        path.add_waypoint(v)
    return path


def path_metrics(mesh, positions, path):
    """Geometry of a routed path: 3D length, how deep in the groove it runs, and
    how much of it rides concave-crease edges (the signal the router optimises)."""
    pts = positions[path.vertices()]
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    edges = path.edges()
    crease = sum(1 for e in edges
                 if mesh.edge_signed_dihedral(e) >= SeamCostParams().crease_degrees)
    return {
        "points": pts,
        "length": float(seg.sum()),
        "arclen": np.concatenate([[0.0], np.cumsum(seg)]),
        "depth": float(-pts[:, 2].mean()),
        "concave_pct": 100.0 * crease / max(1, len(edges)),
        "edges": len(edges),
    }


def editing_demo(mesh, positions):
    """Waypoints stay live after routing: dragging one re-routes only the
    segments that touch it, and a commit is undoable edge-for-edge."""
    angles = [0.0, 80.0, 165.0, 250.0]
    ids = [nearest_vertex(positions, ring_point(a)) for a in angles]
    path = route(mesh, SeamCostParams(), ids)
    revs_before = [path.segment_revision(i) for i in range(path.segment_count())]

    # Drag the last waypoint out of the groove onto the flat outer rim. Only the
    # final segment touches it, so only that segment's revision may move.
    dragged = path.move_waypoint_to(3, ring_point(250.0, radius=0.92, z=0.0), 0.2)
    revs_after = [path.segment_revision(i) for i in range(path.segment_count())]

    seams = SeamSet()
    marked = path.commit(seams)
    committed = len(seams)
    # A committed seam is a SET OF EDGE IDS and nothing else. Mesh.edge_endpoints
    # turns each id back into a vertex pair and Mesh.vertex_position into two
    # points, which is what makes a committed seam drawable at all — the routed
    # path's own vertex list is gone the moment the path is closed.
    seam_segments = committed_segments(mesh, seams)
    seams.revert_commit(marked)
    reverted = len(seams)
    marker = path.resume_marker
    seams.close()
    path.close()
    return {
        "waypoints": len(ids), "segments": len(revs_before),
        "revs_before": revs_before, "revs_after": revs_after, "dragged": dragged,
        "marked": len(marked), "committed": committed, "reverted": reverted,
        "marker": marker, "seam_segments": seam_segments,
    }


def committed_segments(mesh, seams):
    """The committed seam as an ``(n, 2, 3)`` array of world-space segments."""
    segments = []
    for edge in seams.edges():
        v0, v1 = mesh.edge_endpoints(edge)
        segments.append((mesh.vertex_position(v0), mesh.vertex_position(v1)))
    return np.asarray(segments, dtype=float)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
_FLAT = np.array([0.86, 0.88, 0.91])
_DEEP = np.array([0.33, 0.39, 0.47])
_ROUTED = "#e0393e"
_SHORTEST = "#f0a020"
_COMMITTED = "#3f7fd0"


def surface_polys(mesh_data):
    """Triangles plus a per-face colour that darkens with depth, so the trench
    reads as a ring even in a flat-shaded matplotlib view."""
    pos = mesh_data["positions"]
    polys, colors = [], []
    for f in mesh_data["faces"]:
        p = pos[f]
        polys.append(p)
        t = min(1.0, -float(p[:, 2].mean()) / GROOVE_DEPTH)
        colors.append(tuple(_FLAT * (1.0 - t) + _DEEP * t))
    return polys, colors


def draw_surface(ax, mesh_data, paths, elev, azim, title, lift=0.03, seam=None):
    polys, colors = surface_polys(mesh_data)
    ax.add_collection3d(Poly3DCollection(polys, facecolors=colors,
                                         edgecolors=(0.1, 0.13, 0.18, 0.10), linewidths=0.08))
    if seam is not None and len(seam):
        # Drawn edge by edge from the committed SeamSet, resolved through
        # Mesh.edge_endpoints — no routed polyline involved.
        for i, (a, b) in enumerate(seam):
            ax.plot([a[0], b[0]], [a[1], b[1]], [a[2] + lift, b[2] + lift],
                    color=_COMMITTED, linewidth=4.5, solid_capstyle="round", zorder=5,
                    label="committed seam (from edge ids)" if i == 0 else None)
    for label, color, m in paths:
        p = m["points"]
        # Lift the polyline clear of the surface: matplotlib has no depth buffer,
        # so a curve lying exactly in the faces would be diced by them.
        ax.plot(p[:, 0], p[:, 1], p[:, 2] + lift, color=color, linewidth=3.0,
                solid_capstyle="round", label=label, zorder=6)
    ends = paths[0][2]["points"]
    for p, name in ((ends[0], "A"), (ends[-1], "B")):
        ax.scatter([p[0]], [p[1]], [p[2] + lift], s=70, color="#12233a",
                   edgecolors="white", linewidths=1.0, zorder=8)
        ax.text(p[0] * 1.16, p[1] * 1.16, p[2] + lift, name, fontsize=11,
                fontweight="bold", color="#12233a", ha="center", va="center", zorder=9)
    # The plate is nearly flat, so a cubic box would render it as a thin sliver:
    # squash the z axis of the box instead and keep the whole disk in frame.
    ax.set_xlim(-1.02, 1.02)
    ax.set_ylim(-1.02, 1.02)
    ax.set_zlim(-0.30, 0.30)
    ax.set_box_aspect((1, 1, 0.30), zoom=1.32)
    ax.set_axis_off()
    ax.view_init(elev=elev, azim=azim)
    ax.set_title(title, fontsize=11, color="#12233a", pad=-4)


def draw_profile(ax, paths):
    ax.axhline(GROOVE_DEPTH, color="#b6bdc7", linestyle="--", linewidth=1.0, zorder=1)
    ax.text(0.02, GROOVE_DEPTH, " groove floor", va="bottom", fontsize=9, color="#67707d")
    ax.axhline(0.0, color="#b6bdc7", linestyle=":", linewidth=1.0, zorder=1)
    ax.text(0.02, 0.0, " flat plate", va="bottom", fontsize=9, color="#67707d")
    for label, color, m in paths:
        ax.plot(m["arclen"], -m["points"][:, 2], color=color, linewidth=2.4,
                label=label, zorder=3)
    ax.set_xlabel("distance along the path", fontsize=9.5)
    ax.set_ylabel("depth below the plate", fontsize=9.5)
    ax.set_ylim(-0.02, GROOVE_DEPTH * 1.4)
    ax.set_title("depth along each path — the routed one never leaves the trench",
                 fontsize=11, color="#12233a")
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)


def main():
    c.require_engine()
    tmp = tempfile.mkdtemp()
    src = os.path.join(tmp, "grooved_disk.obj")
    grooved_disk_obj(src)

    with Mesh.load_obj(src) as mesh:
        positions = mesh.positions.astype(float)
        # SeamPath speaks vertex ids; `positions` is a compacted snapshot, which
        # matches those ids only while nothing has been deleted (true here).
        assert mesh.vertex_count == len(positions)

        start = nearest_vertex(positions, ring_point(0.0))
        end = nearest_vertex(positions, ring_point(165.0))

        defaults = SeamCostParams.defaults()
        uniform = SeamCostParams(flat_weight=1.0, feature_weight=1.0, concave_weight=1.0,
                                 convex_weight=1.0)
        variants = [("routed (valley-biased)", _ROUTED, defaults),
                    ("shortest path (uniform cost)", _SHORTEST, uniform)]

        paths = []
        for label, color, params in variants:
            path = route(mesh, params, [start, end])
            paths.append((label, color, path_metrics(mesh, positions, path)))
            path.close()

        edit = editing_demo(mesh, positions)

        render_path = os.path.join(tmp, "render.obj")
        mesh.save_obj(render_path)
    mesh_data = c.load_obj(render_path)

    print(f"grooved disk: {len(positions)} verts, {len(mesh_data['faces'])} tris "
          f"— circular V-trench at r={RING_RADIUS}, depth {GROOVE_DEPTH}")
    print("waypoints: two vertices on the groove floor, 165 deg apart\n")
    print(f"{'cost weights':<30}{'flat':>7}{'feature':>9}{'concave':>9}{'crease':>8}")
    print("-" * 63)
    for label, _, params in variants:
        print(f"{label:<30}{params.flat_weight:>7.2f}{params.feature_weight:>9.2f}"
              f"{params.concave_weight:>9.2f}{params.crease_degrees:>7.0f}°")
    print()
    print(f"{'path':<30}{'verts':>7}{'edges':>7}{'length':>9}{'mean depth':>12}"
          f"{'concave edges':>15}")
    print("-" * 80)
    for label, _, m in paths:
        print(f"{label:<30}{len(m['points']):>7}{m['edges']:>7}{m['length']:>9.3f}"
              f"{m['depth']:>12.4f}{m['concave_pct']:>14.0f}%")
    print("-" * 80)
    ratio = paths[0][2]["length"] / max(1e-9, paths[1][2]["length"])
    print(f"the routed seam is {ratio:.2f}x longer in 3D yet cheaper to the router: "
          f"riding concave edges at {defaults.concave_weight:.2f}x")
    print("beats the flat shortcut. mean depth ~= the groove floor means it stayed in "
          "the trench.\n")

    print(f"editing: {edit['waypoints']} waypoints -> {edit['segments']} segments")
    print(f"  segment revisions before drag  {edit['revs_before']}")
    print(f"  move_waypoint_to(3, flat rim) -> {edit['dragged']}")
    print(f"  segment revisions after drag   {edit['revs_after']}  "
          f"(only the touched segment re-routed)")
    print(f"  commit -> {edit['marked']} edges marked, seam set holds {edit['committed']}")
    seam_len = float(np.linalg.norm(np.diff(edit["seam_segments"], axis=1), axis=-1).sum())
    print(f"  edge_endpoints -> {len(edit['seam_segments'])} drawable segments, "
          f"{seam_len:.3f} total length")
    print(f"  revert_commit -> seam set holds {edit['reverted']}")
    print(f"  resume_marker armed at vertex {edit['marker']}")

    fig = plt.figure(figsize=(14.4, 5.2), dpi=130)
    fig.suptitle("Auto-routed seam paths — the seam follows the groove, "
                 "the shortest path cuts across it",
                 fontsize=14, fontweight="bold", color="#12233a", y=0.985)
    ax = fig.add_subplot(1, 3, 1, projection="3d")
    draw_surface(ax, mesh_data, paths, 32, -62,
                 f"grooved plate — A to B, {paths[0][2]['concave_pct']:.0f}% of the "
                 f"routed seam\nrides concave edges vs {paths[1][2]['concave_pct']:.0f}%")
    ax2 = fig.add_subplot(1, 3, 2, projection="3d")
    draw_surface(ax2, mesh_data, paths, 89, -90,
                 f"seen from above — the trench is the dark ring\n"
                 f"blue: the COMMITTED seam, {len(edit['seam_segments'])} edge ids "
                 f"resolved to segments",
                 seam=edit["seam_segments"])
    handles, labels = ax2.get_legend_handles_labels()
    fig.legend(handles, labels, fontsize=10, ncol=2, loc="lower center",
               bbox_to_anchor=(0.34, -0.01), framealpha=0.95)
    ax3 = fig.add_subplot(1, 3, 3)
    draw_profile(ax3, paths)
    fig.tight_layout(rect=(0.0, 0.06, 1, 0.93))
    out = os.path.join(c.OUTPUT_DIR, "20_seam_paths.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
