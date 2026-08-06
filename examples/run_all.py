#!/usr/bin/env python3
"""Runs every example, then stitches the results into one gallery PNG.

    examples/run.sh examples/run_all.py
"""

from __future__ import annotations

import importlib
import os
import sys

import matplotlib.pyplot as plt

import common as c
import cyberremesh

# The feature examples. 10-15 are benchmarks and reference comparisons rather than
# features, so they stay out of the gallery (they pull large external meshes).
EXAMPLES = [
    ("01_quad_remesh", "01_quad_remesh.png"),
    ("02_target_density", "02_target_density.png"),
    ("03_adaptivity", "03_adaptivity.png"),
    ("04_sharp_edges", "04_sharp_edges.png"),
    ("05_pure_quads", "05_pure_quads.png"),
    ("06_hole_fill", "06_hole_fill.png"),
    ("07_baking", "07_baking.png"),
    ("08_load_model", "08_load_model.png"),
    ("09_test_models", "09_gallery.png"),  # downloads community test models on demand
    ("16_soft_selection", "16_soft_selection.png"),
    ("17_flow_guides", "17_flow_guides.png"),
    ("18_sculpt_handoff", "18_sculpt_handoff.png"),
    ("19_export_presets", "19_export_presets.png"),  # presets are CLI-only; drives the binary
    ("20_seam_paths", "20_seam_paths.png"),
]


def run_example(module_name: str) -> str | None:
    """Runs one example. Returns None on success, else why it failed.

    Examples bail with ``sys.exit`` on an unmet prerequisite (19 needs the CLI binary,
    which a bindings-only build does not produce). One of those must not cost the
    whole gallery, so failures are collected and reported at the end instead.
    """
    try:
        importlib.import_module(module_name).main()
    except SystemExit as bail:
        if bail.code:
            return f"exited {bail.code}"
    except Exception as err:  # noqa: BLE001 - report and keep going
        return f"{type(err).__name__}: {err}"
    return None


def contact_sheet(produced: list[tuple[str, str]], path: str) -> None:
    """Stitches every example PNG into one column, in the order they were produced."""
    images = [(name, plt.imread(png)) for name, png in produced]
    # Panels differ a lot in shape (07 is a map strip, 18 a wide filmstrip), so give each
    # row the height its own aspect needs rather than a fixed slice everyone must fit in.
    heights = [11.0 * img.shape[0] / img.shape[1] for _, img in images]
    fig = plt.figure(figsize=(11, sum(heights) + 0.55 * len(images)), dpi=110)
    fig.suptitle(
        f"CyberRemesher — Python binding feature gallery (engine {cyberremesh.version()})",
        fontsize=15, fontweight="bold", color="#12233a", y=0.999,
    )
    grid = fig.add_gridspec(len(images), 1, height_ratios=heights)
    for i, (name, img) in enumerate(images):
        ax = fig.add_subplot(grid[i])
        ax.imshow(img)
        ax.set_axis_off()
        ax.set_title(name, fontsize=10, loc="left", color="#5f6b7d")
    fig.tight_layout(rect=(0, 0, 1, 0.995))
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def main() -> int:
    c.require_engine()
    # quad-cover now DEFAULTS to the native (dependency-free) seamless-UV solver, so the
    # gallery showcases it out of the box. Do NOT set CYBER_QUADCOVER_CLI here; set it in the
    # shell to use the vendored Geogram solver instead. (The AutoRemesher reference panels
    # build/call their binary directly, independent of this.)
    print(f"CyberRemesher engine {cyberremesh.version()} — via the Python (ctypes) binding")
    print("quad-cover: default method (Geogram field when built -DCYBER_WITH_QUADCOVER)\n")

    produced, failed = [], []
    for module_name, png in EXAMPLES:
        print(f"* {module_name}")
        reason = run_example(module_name)
        if reason:
            failed.append((module_name, reason))
            print(f"  FAILED — {reason}")
        path = os.path.join(c.OUTPUT_DIR, png)
        if os.path.exists(path):
            produced.append((module_name, path))
        print()

    gallery = os.path.join(c.OUTPUT_DIR, "gallery.png")
    contact_sheet(produced, gallery)
    print(f"gallery -> {os.path.relpath(gallery, os.path.dirname(c.OUTPUT_DIR))}")

    for module_name, reason in failed:
        print(f"failed: {module_name} — {reason}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
