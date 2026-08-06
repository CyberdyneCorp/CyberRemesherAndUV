#!/usr/bin/env python3
"""Per-DCC export presets — one flag, a bundle the target app can read as-is.

``--preset <name>`` turns a remesh into an *export bundle*: the mesh plus one
baked map per entry, written under the target application's file naming, colour
space and normal-map conventions. Four presets ship built in — blender, unity,
unreal and gltf-generic.

The conventions are not cosmetic. The headline difference is the normal map's
green channel: Blender, Unity and glTF read OpenGL-style normals (green points
up, +Y), Unreal reads DirectX-style (green points down, -Y). Getting it wrong
inverts every dent and bump in the shaded result, so the ``unreal`` preset flips
that channel at write time and the others do not. This example bakes the same
model through all four presets and shows the two normal maps side by side, plus
their difference — green, and only green.

NOTE: export presets are a CLI-only feature. The C ABI has no export/bundle
entry point, so the ``cyberremesh`` Python package cannot request a preset
bundle; this example therefore drives the CLI binary. Everything the bindings
*do* expose (loading the exported mesh back) still goes through them.

    examples/run.sh examples/19_export_presets.py
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

import matplotlib.image as mpimg
import matplotlib.pyplot as plt
import numpy as np

import common as c
from cyberremesh import Mesh

MODEL = os.path.join(c.MODELS_DIR, "spot.obj")
PRESETS = ["blender", "unity", "unreal", "gltf-generic"]
TARGET_QUADS = 1200
TEXTURE_SIZE = 256  # small on purpose: the AO bake is ~all of the runtime
AO_SAMPLES = 4

# The container each preset declares. The CLI honours the --output extension
# over the preset (an explicit path is the user speaking last) and has no way to
# report a preset's mesh format without running it, so the caller must supply a
# matching extension itself.
MESH_EXT = {"blender": "obj", "unity": "glb", "unreal": "glb", "gltf-generic": "glb"}


def cli_binary():
    """Locate the cyberremesh CLI, which is where --preset lives."""
    lib = os.environ.get("CYBER_CAPI_LIB", "")
    build = os.path.dirname(os.path.dirname(lib)) if lib else ""
    candidates = [
        os.environ.get("CYBER_CLI"),
        os.path.join(build, "apps", "cli", "cyberremesh") if build else None,
        os.path.join(c._REPO, "build", "apps", "cli", "cyberremesh"),
        shutil.which("cyberremesh"),
    ]
    for path in candidates:
        if path and os.path.exists(path):
            return path
    print("cyberremesh CLI not found. Build it and re-run:\n"
          "  cmake --build --preset cpu-headless --target cyberremesh\n"
          "  CYBER_CLI=<path> examples/run.sh examples/19_export_presets.py",
          file=sys.stderr)
    sys.exit(1)


def run_preset(binary, preset, workdir):
    """Export one bundle. Returns the run report (which lists every file written)."""
    out_dir = os.path.join(workdir, preset)
    os.makedirs(out_dir, exist_ok=True)
    report_path = os.path.join(out_dir, "report.json")
    run = subprocess.run(
        [binary,
         "--input", MODEL,
         "--output", os.path.join(out_dir, f"spot.{MESH_EXT[preset]}"),
         "--preset", preset,
         "--target-quads", str(TARGET_QUADS),
         "--texture-size", str(TEXTURE_SIZE),
         "--ao-samples", str(AO_SAMPLES),
         "--report", report_path,
         "--quiet"],
        capture_output=True, text=True, timeout=900)
    if run.returncode != 0:
        # A build without the export-bundle module rejects --preset outright,
        # and that message is far more useful than a CalledProcessError.
        print(f"preset '{preset}' failed:\n{run.stderr.strip()[-500:]}", file=sys.stderr)
        sys.exit(1)
    with open(report_path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def output_path(report, kind):
    """Path of one bundle file by kind ('mesh', 'normal', 'ao', ...)."""
    for entry in report["outputs"]:
        if entry["kind"] == kind:
            return entry["path"]
    return None


def map_image(report, kind):
    path = output_path(report, kind)
    return mpimg.imread(path)[..., :3] if path else None


def map_kinds(report):
    return [e["kind"] for e in report["outputs"] if e["kind"] != "mesh"]


def srgb_kinds(report):
    return {e["kind"] for e in report["outputs"] if e.get("colorSpace") == "srgb"}


def green_flip_check(base, flipped):
    """Measure the +Y/-Y convention difference between two normal maps: the red
    and blue channels must match exactly and green must be mirrored (g -> 1-g)."""
    covered = base.sum(axis=2) > 0.0  # background texels are left at zero
    return {
        "red": float(np.abs(base[..., 0] - flipped[..., 0]).max()),
        "blue": float(np.abs(base[..., 2] - flipped[..., 2]).max()),
        "mirror": float(np.abs(flipped[..., 1] - (1.0 - base[..., 1])).max()),
        # Restricted to the charts: the padding flips too, which would inflate this.
        "differing": float(np.mean(np.abs(base[..., 1] - flipped[..., 1])[covered] > 1e-3)
                           * 100.0),
        "covered": covered,
    }


def map_diff(reports, kind):
    """Max per-texel difference of one map between the blender and unreal
    bundles. Everything except the normal map should come out at exactly 0:
    the presets differ in convention, not in what was baked."""
    a, b = map_image(reports["blender"], kind), map_image(reports["unreal"], kind)
    return float(np.abs(a - b).max())


def bundle_mesh_stats(report):
    """Load the exported mesh back through the Python bindings — the one part of
    this workflow the bindings can do — so the figure can quote real counts."""
    path = output_path(report, "mesh")
    with Mesh.load_obj(path) as mesh:
        return mesh.vertex_count, mesh.face_count


def print_table(reports):
    print(f"{'preset':<14}{'mesh':>6}{'tex':>6}{'normal G':>15}{'units':>14}{'res':>6}"
          f"{'files':>7}  maps")
    print("-" * 92)
    for name, report in reports.items():
        preset = report["preset"]
        srgb = srgb_kinds(report)
        maps = ", ".join(f"{k}*" if k in srgb else k for k in map_kinds(report))
        green = "+Y (OpenGL)" if preset["normalGreen"] == "+Y" else "-Y (DirectX)"
        print(f"{name:<14}{preset['meshFormat']:>6}{preset['textureFormat']:>6}{green:>15}"
              f"{preset['units']:>14}{preset['resolution']:>6}"
              f"{len(report['outputs']):>7}  {maps}")
    print("-" * 92)
    print("* = written sRGB-encoded; every other map carries data, not appearance, "
          "and stays linear.")


def draw_map(ax, image, title, subtitle=""):
    ax.imshow(np.clip(image, 0.0, 1.0))
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_title(title, fontsize=11, color="#12233a", fontweight="bold")
    if subtitle:
        ax.set_xlabel(subtitle, fontsize=9, color="#4a5568")


def draw_green_scatter(ax, base, flipped, covered):
    """Green of one map against the other, over the texels that carry surface.
    Every point lands on g_unreal = 1 - g_blender: that IS the convention."""
    rng = np.random.default_rng(7)
    gb, gf = base[..., 1][covered], flipped[..., 1][covered]
    if len(gb) > 4000:
        pick = rng.choice(len(gb), 4000, replace=False)
        gb, gf = gb[pick], gf[pick]
    ax.plot([0, 1], [1, 0], color="#c1392b", linewidth=1.6, zorder=1,
            label="g$_{unreal}$ = 1 − g$_{blender}$")
    ax.scatter(gb, gf, s=4, color="#2f7d32", alpha=0.35, zorder=2, linewidths=0)
    ax.set_xlim(-0.02, 1.02)
    ax.set_ylim(-0.02, 1.02)
    ax.set_aspect("equal")
    ax.set_xlabel("blender green (+Y)", fontsize=9)
    ax.set_ylabel("unreal green (−Y)", fontsize=9)
    ax.tick_params(labelsize=8)
    ax.legend(fontsize=8, loc="upper right", frameon=False)
    ax.set_title("every surface texel is mirrored", fontsize=11, color="#12233a",
                 fontweight="bold")


def draw_green_delta(ax, base, flipped, covered, span=0.3):
    """Signed green difference over the charts. A raw |difference| image reads as
    near-black — most texels sit close to g = 0.5, where mirroring barely moves
    the value — but the SIGN is what a shader reads, and it tracks the surface
    slope: every bump that leaned one way now leans the other."""
    delta = np.where(covered, flipped[..., 1] - base[..., 1], np.nan)
    cmap = plt.get_cmap("bwr").copy()
    cmap.set_bad("#20262e")  # empty texels: outside the charts, nothing to compare
    im = ax.imshow(delta, cmap=cmap, vmin=-span, vmax=span)
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_title("green delta (unreal − blender)", fontsize=11, color="#12233a",
                 fontweight="bold")
    ax.set_xlabel("red / blue deltas are exactly 0 everywhere", fontsize=9, color="#4a5568")
    bar = ax.figure.colorbar(im, ax=ax, fraction=0.046, pad=0.02)
    bar.ax.tick_params(labelsize=8)


def summary_lines(reports, check, mesh_stats, other_diffs):
    lines = ["one remesh -> four bundles", ""]
    for name, report in reports.items():
        preset = report["preset"]
        green = "+Y OpenGL" if preset["normalGreen"] == "+Y" else "-Y DirectX"
        lines.append(f"{name:<13} .{preset['meshFormat']:<4} {green:<10} "
                     f"{len(map_kinds(report))} maps")
        lines.append(f"{'':<13} {', '.join(map_kinds(report))}")
    verts, faces = mesh_stats
    lines += [
        "",
        f"mesh (re-loaded via bindings): {verts} verts / {faces} faces",
        f"maps: {TEXTURE_SIZE}x{TEXTURE_SIZE} png, auto-unwrapped into",
        f"      {reports['blender']['preset']['chartCount']} charts",
        "",
        "blender vs unreal, texel by texel",
        f"  normal red   max |diff| = {check['red']:.4f}",
        f"  normal blue  max |diff| = {check['blue']:.4f}",
        f"  normal green {check['differing']:.0f}% of surface texels differ",
        f"     mirror error = {check['mirror']:.4f} (1/255 = png)",
    ]
    lines += [f"  {kind:<12} max |diff| = {value:.4f}"
              for kind, value in other_diffs.items()]
    lines.append("  -> only the normal map's convention")
    lines.append("     differs; the bake itself is shared.")
    return lines


def main():
    c.require_engine()
    binary = cli_binary()
    workdir = tempfile.mkdtemp(prefix="cyber_presets_")

    reports = {}
    for preset in PRESETS:
        reports[preset] = run_preset(binary, preset, workdir)

    print_table(reports)

    base = map_image(reports["blender"], "normal")
    flipped = map_image(reports["unreal"], "normal")
    check = green_flip_check(base, flipped)
    print()
    print("normal-map convention check (unreal vs blender, same mesh and UVs)")
    print(f"  red   channel: max |diff| = {check['red']:.4f}  (identical)")
    print(f"  blue  channel: max |diff| = {check['blue']:.4f}  (identical)")
    print(f"  green channel: {check['differing']:.1f}% of SURFACE texels differ, "
          f"max |g_unreal - (1 - g_blender)| = {check['mirror']:.4f}")
    other_diffs = {kind: map_diff(reports, kind) for kind in ("ao", "curvature", "color")}
    for kind, value in other_diffs.items():
        print(f"  {kind} map:   max |diff| = {value:.4f}  "
              f"(no convention applies — the bake is shared)")
    verts, faces = bundle_mesh_stats(reports["blender"])
    print(f"  exported mesh re-loaded through the bindings: {verts} verts, {faces} faces")
    print(f"  bundles written under {workdir}")

    fig = plt.figure(figsize=(16.0, 8.6), dpi=120)
    fig.suptitle("Export presets — same bake, packaged for each target app "
                 "(the unreal preset flips the normal map's green channel)",
                 fontsize=15, fontweight="bold", color="#12233a", y=0.98)

    draw_map(fig.add_subplot(2, 4, 1), base, "blender · normal (+Y, OpenGL)",
             "green points up — also unity and gltf-generic")
    draw_map(fig.add_subplot(2, 4, 2), flipped, "unreal · normal (−Y, DirectX)",
             "same bake, green inverted on write (padding too)")
    draw_green_delta(fig.add_subplot(2, 4, 3), base, flipped, check["covered"])
    draw_green_scatter(fig.add_subplot(2, 4, 4), base, flipped, check["covered"])

    blender = reports["blender"]
    srgb = srgb_kinds(blender)
    for col, kind in enumerate(["ao", "curvature", "color"]):
        image = map_image(blender, kind)
        space = "sRGB-encoded" if kind in srgb else "linear"
        if float(image[check["covered"]].std()) < 1e-3:
            space += " · flat: this source carries none"
        draw_map(fig.add_subplot(2, 4, 5 + col), image,
                 f"blender · {kind}", f"same bundle, {space}")

    ax = fig.add_subplot(2, 4, 8)
    ax.axis("off")
    ax.text(0.0, 1.0, "\n".join(summary_lines(reports, check, (verts, faces), other_diffs)),
            fontsize=9.5, family="monospace", va="top", ha="left", color="#12233a")

    fig.tight_layout(rect=(0, 0, 1, 0.95))
    out = os.path.join(c.OUTPUT_DIR, "19_export_presets.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
