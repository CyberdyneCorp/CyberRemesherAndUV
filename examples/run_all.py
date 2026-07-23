#!/usr/bin/env python3
"""Runs every example, then stitches the results into one gallery PNG.

    examples/run.sh examples/run_all.py
"""

import importlib
import os

import matplotlib.pyplot as plt

import common as c
import cyberremesh

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
]


def main() -> None:
    c.require_engine()
    # quad-cover now DEFAULTS to the native (dependency-free) seamless-UV solver, so the
    # gallery showcases it out of the box. Do NOT set CYBER_QUADCOVER_CLI here; set it in the
    # shell to use the vendored Geogram solver instead. (The AutoRemesher reference panels
    # build/call their binary directly, independent of this.)
    print(f"CyberRemesher engine {cyberremesh.version()} — via the Python (ctypes) binding")
    print("quad-cover: default method (Geogram field when built -DCYBER_WITH_QUADCOVER)\n")

    produced = []
    for module_name, png in EXAMPLES:
        print(f"* {module_name}")
        importlib.import_module(module_name).main()
        path = os.path.join(c.OUTPUT_DIR, png)
        if os.path.exists(path):
            produced.append((module_name, path))
        print()

    # Contact sheet.
    rows = len(produced)
    fig = plt.figure(figsize=(11, 3.0 * rows), dpi=110)
    fig.suptitle(
        f"CyberRemesher — Python binding feature gallery (engine {cyberremesh.version()})",
        fontsize=15, fontweight="bold", color="#12233a", y=0.995,
    )
    for i, (name, path) in enumerate(produced):
        ax = fig.add_subplot(rows, 1, i + 1)
        ax.imshow(plt.imread(path))
        ax.set_axis_off()
        ax.set_title(name, fontsize=10, loc="left", color="#5f6b7d")
    fig.tight_layout(rect=(0, 0, 1, 0.99))
    gallery = os.path.join(c.OUTPUT_DIR, "gallery.png")
    fig.savefig(gallery, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"gallery -> {os.path.relpath(gallery, os.path.dirname(c.OUTPUT_DIR))}")


if __name__ == "__main__":
    main()
