#!/usr/bin/env python3
"""Soft selection — a gradient region, a weighted edit, glued to the Target.

The manual-retopology stage has tweak, relax and rigid transforms; soft
selection adds the gradient region on top. A line selection ramps the weight
0 -> 1 along the drag, a weighted scale tapers the mesh by that weight, and the
Target re-projection happens INSIDE the transform: the edited surface never
peels off the sculpt, so there is no snap-all cleanup pass to remember.

This example generates a UV sphere, uses it as both the Target and (a coarser
copy as) the EditMesh, and renders what the falloff actually does:

  * the weight field painted straight onto the mesh (``select_line`` with the
    15-degree angle snap, ``select_sphere``, ``paint_selection_stroke``),
  * the same weighted taper run twice — once free, once with a ``Snapper`` —
    so the falloff is visible as geometry AND as glue,
  * the four ``Falloff`` curves measured back out of the engine,
  * per-vertex movement against weight, which is where you can read off that
    zero-weight geometry did not move at all.

    examples/run.sh examples/16_soft_selection.py
"""

import math
import os
import tempfile

import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

import common as c
from cyberremesh import Falloff, Mesh, Snapper

CMAP = plt.get_cmap("viridis")
IDENTITY = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
TAPER = [0.55, 0, 0, 0, 1, 0, 0, 0, 0.55, 0, 0, 0]  # squeeze x/z, leave y
# The house camera (elev 22, azim 35) looks down this direction; the sphere and
# stroke demos are placed on that side so they face the reader.
VIEW = np.array([math.cos(math.radians(22)) * math.cos(math.radians(35)),
                 math.cos(math.radians(22)) * math.sin(math.radians(35)),
                 math.sin(math.radians(22))])


def histogram(weights, bins=5):
    """Counts of weights per equal-width bin over [0, 1]."""
    counts = [0] * bins
    for w in weights:
        index = min(bins - 1, int(w * bins))
        counts[index] += 1
    return counts


def worst_off_surface(mesh, snapper, weights):
    """Largest distance from an affected vertex to the Target surface."""
    positions = mesh.positions
    worst = 0.0
    for v, w in enumerate(weights):
        if w <= 0.0:
            continue
        # Re-running the snap on an already-glued vertex must move it ~nowhere.
        before = positions[v]
        mesh.set_selection_weights([1.0 if i == v else 0.0 for i in range(len(weights))])
        mesh.transform_selection(IDENTITY, snapper=snapper)
        worst = max(worst, float(np.linalg.norm(mesh.positions[v] - before)))
    return worst


def stroke_samples(count=16, pressure_from=0.35):
    """A brush gesture across the visible side: (x, y, z, pressure) samples."""
    samples = []
    for i in range(count):
        t = i / (count - 1)
        theta = math.radians(35.0) + (t - 0.5) * 2.0
        phi = math.radians(75.0) - 0.55 * math.sin(math.pi * t)
        samples.append((math.sin(phi) * math.cos(theta), math.sin(phi) * math.sin(theta),
                        math.cos(phi), pressure_from + (1.0 - pressure_from) * t))
    return samples


def moved_by_weight(before, after, weights):
    """(per-vertex movement, largest movement of a zero-weight vertex)."""
    delta = np.linalg.norm(after["positions"] - before["positions"], axis=1)
    zeros = delta[np.asarray(weights) <= 0.0]
    return delta, float(zeros.max()) if zeros.size else 0.0


def repose(frame, positions):
    """`frame`'s faces with engine-read positions substituted in.

    Everything the panels draw is topology from the OBJ but geometry straight
    out of the engine, so no comparison here measures the OBJ round trip's
    float32 rounding.
    """
    return {"positions": positions, "faces": frame["faces"]}


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def draw_weighted(ax, mesh, weights, title, drag=None, span=None, cmap=CMAP):
    """A mesh with every face tinted by the mean selection weight of its corners.

    ``drag`` draws the (anchor, end) of a line selection beside the mesh, so the
    gradient direction is visible rather than inferred from the colours. ``span``
    is a shared half-extent so every panel renders at the same scale.
    """
    pos = mesh["positions"]
    faces = mesh["faces"]
    if weights is None:
        colors = ["#c8ccd2"] * len(faces)
    else:
        w = np.asarray(weights, dtype=float)
        colors = [cmap(float(w[f].mean())) for f in faces]
    ax.add_collection3d(Poly3DCollection([[pos[i] for i in f] for f in faces],
                                         facecolors=colors, edgecolors="#20262e",
                                         linewidths=0.2))
    lo, hi = pos.min(axis=0), pos.max(axis=0)
    if drag is not None:
        a, b = np.asarray(drag[0], dtype=float), np.asarray(drag[1], dtype=float)
        ax.quiver(*a, *(b - a), color="#d1495b", linewidth=2.0, arrow_length_ratio=0.16)
        ax.text(*(b + (b - a) * 0.08), "drag", color="#d1495b", fontsize=9)
    ctr = (lo + hi) / 2.0
    span = span or (float((hi - lo).max()) * 0.56 or 1.0)
    for setlim, mid in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), ctr):
        setlim(mid - span, mid + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=22, azim=35)
    ax.set_title(title, fontsize=9.5, color="#12233a", pad=0)


def draw_falloff_curves(ax, curves):
    for name, (t, w) in curves.items():
        ax.plot(t, w, linewidth=1.8, label=name)
    ax.set_xlabel("t along the drag", fontsize=8.5)
    ax.set_ylabel("selection weight", fontsize=8.5)
    ax.set_xlim(0.0, 1.0)
    ax.set_ylim(-0.03, 1.03)
    ax.tick_params(labelsize=8)
    ax.grid(alpha=0.25, linewidth=0.5)
    ax.legend(fontsize=8, frameon=False)
    ax.set_title("Falloff curves, read back from the engine\n"
                 "(select_line weights vs position along the axis)",
                 fontsize=9.5, color="#12233a")


def draw_move_vs_weight(ax, weights, free_delta, snap_delta):
    w = np.asarray(weights, dtype=float)
    ax.scatter(w, free_delta, s=9, color="#ed9b40", label="free (no Target)")
    ax.scatter(w, snap_delta, s=9, color="#5b9bd5", label="snapped to Target")
    ax.axvline(0.0, color="#98a0ad", linewidth=0.8, linestyle="--")
    ax.set_xlabel("selection weight", fontsize=8.5)
    ax.set_ylabel("distance the vertex moved", fontsize=8.5)
    ax.set_xlim(-0.05, 1.05)
    ax.tick_params(labelsize=8)
    ax.grid(alpha=0.25, linewidth=0.5)
    ax.legend(fontsize=8, frameon=False, loc="upper left")
    ax.set_title("Movement vs weight — smooth ramp, and every\n"
                 "zero-weight vertex sits exactly on 0 (the vertical spread is\n"
                 "the vertex's distance from the taper axis: poles move least)",
                 fontsize=9.5, color="#12233a")


def render(panels, curves, weights, free_delta, snap_delta, path):
    """Six mesh panels around the two measurement plots (grid slots 5 and 6)."""
    fig = plt.figure(figsize=(18.4, 9.6), dpi=120)
    fig.suptitle("Soft selection — gradient regions, weighted edits, glued to the Target",
                 fontsize=15, fontweight="bold", color="#12233a", y=0.985)
    extent = panels[0][0]["positions"]
    span = float((extent.max(axis=0) - extent.min(axis=0)).max()) * 0.64
    for slot, (mesh, w, title, drag) in zip((1, 2, 3, 4, 7, 8), panels):
        draw_weighted(fig.add_subplot(2, 4, slot, projection="3d"), mesh, w, title, drag, span)
    draw_falloff_curves(fig.add_subplot(2, 4, 5), curves)
    draw_move_vs_weight(fig.add_subplot(2, 4, 6), weights, free_delta, snap_delta)
    fig.tight_layout(rect=(0, 0.055, 1, 0.955))

    bar = fig.add_axes((0.34, 0.028, 0.32, 0.016))
    fig.colorbar(plt.cm.ScalarMappable(cmap=CMAP), cax=bar, orientation="horizontal")
    bar.set_xlabel("selection weight (0 = untouched, 1 = full strength)", fontsize=9,
                   color="#12233a")
    bar.tick_params(labelsize=8)

    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(path, c._REPO)}")


# ---------------------------------------------------------------------------
def main() -> None:
    c.require_engine()

    tmp = tempfile.mkdtemp(prefix="cyber_soft_")
    target_path = os.path.join(tmp, "target.obj")
    edit_path = os.path.join(tmp, "edit.obj")
    glued_path = os.path.join(tmp, "glued.obj")
    c.uv_sphere_obj(target_path, rings=48, segments=96)
    c.uv_sphere_obj(edit_path, rings=12, segments=24)

    with Mesh.load_obj(target_path) as target, Mesh.load_obj(edit_path) as edit:
        with Snapper(target) as snapper:
            # Glue the coarse EditMesh onto the Target first (weight 1 everywhere,
            # identity transform = a pure re-projection through the same op).
            edit.set_selection_weights([1.0] * edit.vertex_count)
            edit.transform_selection(IDENTITY, snapper=snapper)
            edit.save_obj(glued_path)
            # Faces from the OBJ, geometry from the engine: Mesh.positions is a
            # lossless read, the OBJ text is not.
            before = repose(c.load_obj(glued_path), edit.positions.copy())

            # The 15-degree angle snap: a drag 7 degrees off the +y axis (in the
            # view plane, view_dir = +z) quantizes back onto the axis, so its
            # weights must match an exactly-axis-aligned drag.
            tilt = math.radians(7.0)
            edit.select_line((0.0, 0.0, 0.0), (math.sin(tilt), math.cos(tilt), 0.0),
                             view_dir=(0.0, 0.0, 1.0), snap_angle=True, snap_degrees=15.0,
                             falloff=Falloff.SMOOTH)
            snapped_line = np.asarray(edit.selection_weights())
            edit.select_line((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), falloff=Falloff.SMOOTH)
            snap_error = float(np.abs(snapped_line - np.asarray(edit.selection_weights())).max())

            # Line gradient up the +y axis: 0 at the origin plane, 1 at +y.
            edit.smooth_selection(2)
            weights = edit.selection_weights()
            edit.save_selection("north-taper")

            # The same weighted taper twice: free, then with the Target attached.
            # Auto re-snap is part of the second call, not a follow-up pass.
            # Mesh.copy duplicates the handle in memory — weight field included —
            # so the two runs start from bit-identical geometry.
            with edit.copy() as free:
                free_report = free.transform_selection(TAPER)
                after_free = repose(before, free.positions.copy())
            report = edit.transform_selection(TAPER, snapper=snapper)
            after_snap = repose(before, edit.positions.copy())

            free_delta, free_zero = moved_by_weight(before, after_free, weights)
            snap_delta, snap_zero = moved_by_weight(before, after_snap, weights)

            print("CyberRemesher soft selection")
            print("  slots:            ", edit.selection_slots())
            print("  weight histogram: ", histogram(weights))
            print("  vertices moved:   ", report.moved, "of", edit.vertex_count)
            # moved - resnapped is not a leak: those vertices were already on the
            # Target, so the re-projection had nothing to correct.
            print("  re-snapped:       ", report.resnapped, "of", report.moved,
                  "(max pull {0:.4f}, {1} needed no correction)".format(
                      report.max_snap_distance, report.moved - report.resnapped))
            print("  15-deg snap:       7-deg drag == axis drag "
                  "(max weight diff {0:.1e})".format(snap_error))
            print("  free taper:        moved {0}, max {1:.3f}, "
                  "zero-weight verts moved {2:.2e}".format(
                      free_report.moved, float(free_delta.max()), free_zero))
            print("  snapped taper:     max {0:.3f}, zero-weight verts moved {1:.2e}".format(
                float(snap_delta.max()), snap_zero))

            edit.load_selection("north-taper")
            print("  worst off-surface after the taper: {0:.2e}".format(
                worst_off_surface(edit, snapper, weights)))

            # The other two region tools, on a denser copy so the brush footprint
            # is resolved by the mesh rather than by the quad size.
            region_path = os.path.join(tmp, "region.obj")
            c.uv_sphere_obj(region_path, rings=20, segments=40)
            with Mesh.load_obj(region_path) as probe:
                region_before = repose(c.load_obj(region_path), probe.positions.copy())
                center = VIEW / np.linalg.norm(VIEW)
                probe.select_sphere(center, 0.9, falloff=Falloff.SMOOTH)
                sphere_weights = probe.selection_weights()

                probe.clear_selection()
                probe.paint_selection_stroke(stroke_samples(), radius=0.30,
                                             falloff=Falloff.SMOOTH)
                stroke_weights = probe.selection_weights()
                relax = probe.relax_selection(strength=0.7, iterations=12, snapper=snapper)
                after_relax = repose(region_before, probe.positions.copy())
                region_count = probe.vertex_count
            _, relax_zero = moved_by_weight(region_before, after_relax, stroke_weights)
            print("  sphere region:     weight>0 on {0} of {1} v".format(
                sum(1 for w in sphere_weights if w > 0.0), region_count))
            # relax.moved is DISTINCT vertices, so it stays inside the mesh even
            # though 12 sweeps wrote each of them 12 times.
            print("  painted stroke:    weight>0 on {0} v, relax moved {1} of {2} v over "
                  "12 iters (zero-weight verts moved {3:.2e})".format(
                      sum(1 for w in stroke_weights if w > 0.0), relax.moved, region_count,
                      relax_zero))

            # The four falloff curves, measured on the dense Target: select_line
            # along +y makes the weight a pure function of t = (y + 1) / 2.
            curves = {}
            for name, mode in (("LINEAR", Falloff.LINEAR), ("SMOOTH", Falloff.SMOOTH),
                               ("SHARP", Falloff.SHARP), ("ROUND", Falloff.ROUND)):
                target.select_line((0.0, -1.0, 0.0), (0.0, 1.0, 0.0), falloff=mode)
                t = (target.positions[:, 1] + 1.0) / 2.0
                order = np.argsort(t)
                curves[name] = (t[order], np.asarray(target.selection_weights())[order])

            out = os.path.join(c.OUTPUT_DIR, "16_soft_selection.obj")
            os.makedirs(c.OUTPUT_DIR, exist_ok=True)
            edit.save_obj(out)
            print("  wrote", out)

            active = int(np.count_nonzero(np.asarray(weights) > 0.0))
            drag = ((0.0, -1.0, -1.2), (0.0, 1.0, -1.2))  # under the mesh, along +y
            panels = [
                (before, None, f"1. EditMesh glued to the Target\n"
                               f"{edit.vertex_count} v · grey = no selection yet", None),
                (before, weights, f"2. select_line + smooth×2 — the weight field\n"
                                  f"weight>0 on {active} of {edit.vertex_count} v · "
                                  f"15° snap exact (Δ {snap_error:.0e})", drag),
                (after_free, weights, f"3. transform_selection — free taper\n"
                                      f"moved {free_report.moved} v · "
                                      f"w=0 moved {free_zero:.1e}", None),
                (after_snap, weights, f"4. transform_selection(snapper=Target)\n"
                                      f"re-snapped {report.resnapped} of {report.moved} moved · "
                                      f"the other {report.moved - report.resnapped} were\n"
                                      f"already on the Target · stays on it, slides instead",
                 None),
                (region_before, sphere_weights,
                 "7. select_sphere(r=0.9) — radial falloff\n"
                 "the same weight field, a different region tool", None),
                (region_before, stroke_weights,
                 f"8. paint_selection_stroke → relax_selection\n"
                 f"weighted relax, 12 iters · w=0 moved {relax_zero:.1e}", None),
            ]
            render(panels, curves, weights, free_delta, snap_delta,
                   os.path.join(c.OUTPUT_DIR, "16_soft_selection.png"))


if __name__ == "__main__":
    main()
