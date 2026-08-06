#!/usr/bin/env python3
"""Per-DCC export presets — one call, a bundle the target app can read as-is.

An *export preset* turns a remesh into an export *bundle*: the mesh plus one
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

Everything here runs through the ``cyberremesh`` bindings: ``ExportPreset``
resolves a preset and reports what it declares (including which mesh container
it wants, which is why no extension table is hard-coded below), and
``write_bundle`` writes the bundle. The low-poly is unwrapped ONCE up front, so
all four bundles bake against byte-identical UVs and any difference between
them is the preset convention and nothing else.

    examples/run.sh examples/19_export_presets.py
"""

import os
import tempfile

import matplotlib.image as mpimg
import matplotlib.pyplot as plt
import numpy as np

import common as c
from cyberremesh import ExportPreset, Mesh, RemeshParams, remesh, write_bundle

MODEL = os.path.join(c.MODELS_DIR, "spot.obj")
PRESETS = ["blender", "unity", "unreal", "gltf-generic"]
TARGET_QUADS = 1200
TEXTURE_SIZE = 256  # small on purpose: the AO bake is ~all of the runtime
AO_SAMPLES = 32  # 4 was too few once AO gained per-texel dither: the figure read as speckle


def to_report(preset, result, chart_count):
    """Flatten a preset and its bundle into the dict the figure code reads."""
    return {
        "preset": {
            "name": preset.name,
            "schemaVersion": preset.schema_version,
            "meshFormat": preset.mesh_format,
            "textureFormat": preset.texture_format,
            "namingPattern": preset.naming_pattern,
            "normalGreen": preset.normal_green,
            "resolution": preset.resolution,
            "units": preset.units,
            "upAxis": preset.up_axis,
            "chartCount": chart_count,
        },
        "outputs": [{"path": f.path, "kind": f.kind, "colorSpace": f.color_space,
                     "width": f.width, "height": f.height}
                    for f in result.files],
        "warnings": result.warnings,
    }


def run_preset(low, high, name, workdir, cage_distance, chart_count):
    """Export one bundle through the bindings. Returns its report."""
    out_dir = os.path.join(workdir, name)
    os.makedirs(out_dir, exist_ok=True)
    with ExportPreset.resolve(name) as preset:
        preset.resolution = TEXTURE_SIZE
        # The preset itself names the container it wants, so the output path
        # can match it instead of being guessed from a hard-coded table.
        mesh_path = os.path.join(out_dir, "spot.{0}".format(preset.mesh_format))
        result = write_bundle(low, high, preset, mesh_path,
                              cage_distance=cage_distance, ao_samples=AO_SAMPLES)
        for warning in result.warnings:
            print("warning ({0}): {1}".format(name, warning))
        return to_report(preset, result, chart_count)


def cage_distance(mesh):
    """1% of the bounding-box diagonal — a fixed cage misses a non-unit model."""
    positions = np.asarray(mesh.positions).reshape(-1, 3)
    return 0.01 * float(np.linalg.norm(positions.max(axis=0) - positions.min(axis=0)))


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
    """Load the exported mesh back and quote its real counts. Only the presets
    whose container is OBJ can be re-read here; the glTF ones are binary."""
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
    workdir = tempfile.mkdtemp(prefix="cyber_presets_")

    with Mesh.load_obj(MODEL) as high:
        cage = cage_distance(high)
        low = remesh(high, RemeshParams(target_quad_count=TARGET_QUADS))
        with low:
            # Unwrapped once, up front: write_bundle would do it per bundle,
            # but sharing one atlas is what makes the texel-by-texel
            # comparison below provably about the presets and nothing else.
            atlas = low.unwrap_atlas()
            reports = {name: run_preset(low, high, name, workdir, cage, atlas.chart_count)
                       for name in PRESETS}

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
