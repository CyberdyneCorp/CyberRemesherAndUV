#!/usr/bin/env python3
"""Soft selection — taper a region by a line gradient, glued to the Target.

The manual-retopology stage has tweak, relax and rigid transforms; soft
selection adds the gradient region on top. A line selection ramps the weight
0 -> 1 along the drag, a weighted scale tapers the mesh by that weight, and the
Target re-projection happens INSIDE the transform: the edited surface never
peels off the sculpt, so there is no snap-all cleanup pass to remember.

This example generates a UV sphere, uses it as both the Target and (a coarser
copy as) the EditMesh, applies a line-gradient taper, and prints the weight
histogram plus the worst off-surface distance afterwards.

    examples/run.sh examples/16_soft_selection.py
"""

import math
import os
import tempfile

import common as c
from cyberremesh import Falloff, Mesh, Snapper


def histogram(weights, bins=5):
    """Counts of weights per equal-width bin over [0, 1]."""
    counts = [0] * bins
    for w in weights:
        index = min(bins - 1, int(w * bins))
        counts[index] += 1
    return counts


def worst_off_surface(mesh, snapper, weights):
    """Largest distance from an affected vertex to the Target surface."""
    positions = list(mesh._copy_positions())
    worst = 0.0
    for v, w in enumerate(weights):
        if w <= 0.0:
            continue
        # Re-running the snap on an already-glued vertex must move it ~nowhere.
        before = positions[v * 3:v * 3 + 3]
        mesh.set_selection_weights([1.0 if i == v else 0.0 for i in range(len(weights))])
        mesh.transform_selection([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0], snapper=snapper)
        after = list(mesh._copy_positions())[v * 3:v * 3 + 3]
        worst = max(worst, math.dist(before, after))
    return worst


def main() -> None:
    c.require_engine()

    target_path = os.path.join(tempfile.gettempdir(), "cyber_soft_target.obj")
    edit_path = os.path.join(tempfile.gettempdir(), "cyber_soft_edit.obj")
    c.uv_sphere_obj(target_path, rings=48, segments=96)
    c.uv_sphere_obj(edit_path, rings=12, segments=24)

    with Mesh.load_obj(target_path) as target, Mesh.load_obj(edit_path) as edit:
        with Snapper(target) as snapper:
            # Glue the coarse EditMesh onto the Target first (weight 1 everywhere,
            # identity transform = a pure re-projection through the same op).
            edit.set_selection_weights([1.0] * edit.vertex_count)
            edit.transform_selection(
                [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0], snapper=snapper
            )

            # Line gradient up the +y axis: 0 at the equator, 1 at the pole.
            edit.select_line((0.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                             falloff=Falloff.SMOOTH)
            edit.smooth_selection(2)
            weights = edit.selection_weights()
            edit.save_selection("north-taper")

            # Taper: squeeze x/z by the weight. Auto re-snap is part of the call.
            report = edit.transform_selection(
                [0.55, 0, 0, 0, 1, 0, 0, 0, 0.55, 0, 0, 0],
                snapper=snapper,
            )

            print("CyberRemesher soft selection")
            print("  slots:            ", edit.selection_slots())
            print("  weight histogram: ", histogram(weights))
            print("  vertices moved:   ", report.moved, "of", edit.vertex_count)
            print("  re-snapped:       ", report.resnapped,
                  "(max pull {0:.4f})".format(report.max_snap_distance))

            edit.load_selection("north-taper")
            print("  worst off-surface after the taper: {0:.2e}".format(
                worst_off_surface(edit, snapper, weights)))

            out = os.path.join(c.OUTPUT_DIR, "16_soft_selection.obj")
            os.makedirs(c.OUTPUT_DIR, exist_ok=True)
            edit.save_obj(out)
            print("  wrote", out)


if __name__ == "__main__":
    main()
