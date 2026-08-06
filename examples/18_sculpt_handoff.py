#!/usr/bin/env python3
"""Sculpt handoff bridge: a synthetic producer, then sculpt-to-asset in-process.

The product story is `sculpt -> handoff -> remesh -> UV -> bake`, plus the step
that makes it survive contact with a real artist: `conform`, which re-snaps the
retopology after the sculpt keeps moving. The engine's half is the *receiving*
one: it reads a versioned sculpt handoff (docs/sculpt-handoff-format.md) and
never links, or even knows about, whichever engine produced it.

There is no sculpting engine in this repository, so this script plays the
producer: it emits a handoff PLY for a lumpy blob, then drives the whole
pipeline through the Python bindings — ``Mesh.load_handoff``, ``remesh``,
``unwrap_atlas``, ``bake`` / ``bake_field`` and ``conform``. Any producer that
emits the same format is equally a first-class citizen.

    examples/run.sh examples/18_sculpt_handoff.py            # full pipeline + figure
    python examples/18_sculpt_handoff.py --emit blob.ply     # just write a handoff
    python examples/18_sculpt_handoff.py --emit -            # handoff to stdout

The producer half (``blob`` / ``write_handoff``) stays dependency-free, so
``--emit`` still runs on a bare interpreter with no numpy, matplotlib or engine.
"""

import argparse
import math
import os
import sys
import tempfile

try:  # the producer half must stay runnable on a bare interpreter
    import numpy as np
    import matplotlib.pyplot as plt
    from matplotlib import cm, colors as mcolors
    from matplotlib.gridspec import GridSpec
    from matplotlib.patches import Polygon as MplPolygon
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    import common
except ImportError:  # only --emit works without them
    np = plt = cm = mcolors = GridSpec = MplPolygon = Poly3DCollection = common = None

HANDOFF_MAJOR = 1
HANDOFF_MINOR = 0

# Where the artist adds the second-pass lump, and how strong it is. The second
# handoff is the same blob with this bulge — that is the change `conform` has to
# absorb without touching the retopology's topology or its UVs.
BULGE_DIR = (0.86, 0.30, 0.42)
BULGE_WIDTH = 0.65  # radians of angular falloff
BULGE_HEIGHT = 0.30

# Deviation above which conform flags a vertex: "this one stretched, look at it".
CONFORM_THRESHOLD = 0.05


def _bulge(direction, amount):
    """Radial multiplier of a soft, localised second sculpt pass."""
    if amount <= 0.0:
        return 1.0
    norm = math.sqrt(sum(v * v for v in BULGE_DIR))
    dot = sum(a * b for a, b in zip(direction, BULGE_DIR)) / norm
    angle = math.acos(max(-1.0, min(1.0, dot)))
    return 1.0 + amount * math.exp(-((angle / BULGE_WIDTH) ** 2))


def blob(rings=20, segments=40, radius=1.0, bulge=0.0):
    """A lumpy sphere: positions, normals, colors, material mix, triangles.

    `bulge` re-runs the sculpt: it grows one localised lump on the surface, so
    two calls model "the artist kept working after retopology started"."""
    verts = []
    for j in range(1, rings):  # skip the poles: no degenerate fan triangles
        theta = math.pi * j / rings
        for i in range(segments):
            phi = 2.0 * math.pi * i / segments
            nx = math.sin(theta) * math.cos(phi)
            ny = math.sin(theta) * math.sin(phi)
            nz = math.cos(theta)
            bump = 1.0 + 0.12 * math.sin(5.0 * phi) * math.sin(4.0 * theta)
            r = radius * bump * _bulge((nx, ny, nz), bulge)
            color = (0.5 + 0.5 * nx, 0.5 + 0.5 * ny, 0.5 + 0.5 * nz)
            mix = 0.5 + 0.5 * math.sin(3.0 * theta)
            verts.append(((nx * r, ny * r, nz * r), (nx, ny, nz), color, mix))

    # Poles, added last so the ring indexing above stays simple.
    north = len(verts)
    verts.append(((0.0, 0.0, radius), (0.0, 0.0, 1.0), (1.0, 1.0, 1.0), 0.5))
    south = len(verts)
    verts.append(((0.0, 0.0, -radius), (0.0, 0.0, -1.0), (1.0, 1.0, 1.0), 0.5))

    tris = []
    for j in range(rings - 2):
        for i in range(segments):
            nxt = (i + 1) % segments
            a = j * segments + i
            b = j * segments + nxt
            c = (j + 1) * segments + nxt
            d = (j + 1) * segments + i
            tris.append((a, c, b))
            tris.append((a, d, c))
    for i in range(segments):
        nxt = (i + 1) % segments
        tris.append((north, i, nxt))
        last = (rings - 2) * segments
        tris.append((south, last + nxt, last + i))
    return verts, tris


def write_handoff(stream, verts, tris, major=HANDOFF_MAJOR, minor=HANDOFF_MINOR,
                  producer="examples/18_sculpt_handoff.py"):
    """Emits the ASCII PLY profile from docs/sculpt-handoff-format.md."""
    out = ["ply", "format ascii 1.0",
           f"comment cyber_sculpt_handoff {major} {minor}",
           f"comment cyber_handoff_producer {producer}",
           f"element vertex {len(verts)}",
           "property float x", "property float y", "property float z",
           "property float nx", "property float ny", "property float nz",
           "property uchar red", "property uchar green", "property uchar blue",
           "property float material_mix",
           f"element face {len(tris)}",
           "property list uchar int vertex_indices",
           "end_header"]
    for (p, n, c, mix) in verts:
        rgb = tuple(max(0, min(255, int(round(v * 255)))) for v in c)
        out.append(f"{p[0]:.6f} {p[1]:.6f} {p[2]:.6f} {n[0]:.6f} {n[1]:.6f} {n[2]:.6f} "
                   f"{rgb[0]} {rgb[1]} {rgb[2]} {mix:.4f}")
    for t in tris:
        out.append(f"3 {t[0]} {t[1]} {t[2]}")
    stream.write("\n".join(out) + "\n")


def emit_handoff(path, verts, tris, **kwargs):
    """Write a handoff PLY and return its path."""
    with open(path, "w", encoding="utf-8") as fh:
        write_handoff(fh, verts, tris, **kwargs)
    return path


def show_handoff(label, info):
    """Print what the engine read out of the handoff header."""
    print(f"  {label}")
    print(f"    handoff version  {info.version[0]}.{info.version[1]}")
    print(f"    producer         {info.producer}")
    print(f"    geometry         {info.vertex_count} verts / {info.face_count} faces")
    print(f"    vertex colors    {info.has_vertex_colors}")
    print(f"    vertex normals   {info.has_vertex_normals}")
    print(f"    material mix     {info.has_material_mix}")


# ---------------------------------------------------------------------------
# Figure (engine-dependent half)
# ---------------------------------------------------------------------------
def load_obj_uv(path):
    """Parse an OBJ into (positions, uvs, face_v_indices, face_vt_indices)."""
    pos, uvs, faces_v, faces_vt = [], [], [], []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            p = line.split()
            if not p:
                continue
            if p[0] == "v" and len(p) >= 4:
                pos.append((float(p[1]), float(p[2]), float(p[3])))
            elif p[0] == "vt" and len(p) >= 3:
                uvs.append((float(p[1]), float(p[2])))
            elif p[0] == "f":
                faces_v.append([int(t.split("/")[0]) - 1 for t in p[1:]])
                faces_vt.append([int(t.split("/")[1]) - 1 if "/" in t else -1 for t in p[1:]])
    return (np.array(pos, dtype=float), np.array(uvs, dtype=float), faces_v, faces_vt)


def uv_charts(uvs, faces_vt):
    """Chart index per face: union-find over faces that share a UV coordinate.

    Faces inside a chart share exact UVs at shared corners; across a seam the
    two sides get different UVs, so the charts never merge. (The OBJ writer
    emits a distinct vt per corner, so UVs are compared by value, not index.)"""
    parent = list(range(len(faces_vt)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    seen = {}
    for fi, vts in enumerate(faces_vt):
        for vt in vts:
            if vt < 0:
                continue
            key = (round(float(uvs[vt][0]), 6), round(float(uvs[vt][1]), 6))
            other = seen.setdefault(key, fi)
            ra, rb = find(other), find(fi)
            if ra != rb:
                parent[max(ra, rb)] = min(ra, rb)

    roots = [find(i) for i in range(len(faces_vt))]
    relabel = {r: i for i, r in enumerate(sorted(set(roots)))}
    return [relabel[r] for r in roots], len(relabel)


def draw_3d(ax, pos, faces, colors, title):
    coll = Poly3DCollection([[pos[i] for i in f] for f in faces], facecolors=colors,
                            edgecolors="#20262e", linewidths=0.2)
    ax.add_collection3d(coll)
    lo, hi = pos.min(0), pos.max(0)
    ctr, span = (lo + hi) / 2.0, float((hi - lo).max()) * 0.5 or 1.0
    for setlim, ctr_c in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), ctr):
        setlim(ctr_c - span, ctr_c + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=22, azim=35)
    ax.set_title(title, fontsize=9.5, color="#12233a", pad=0)


def draw_atlas(ax, uvs, faces_vt, colors, title):
    for f, col in zip(faces_vt, colors):
        if any(i < 0 for i in f):
            continue
        ax.add_patch(MplPolygon([uvs[i] for i in f], closed=True, facecolor=col,
                                edgecolor="#2a2f3a", linewidth=0.15))
    ax.add_patch(MplPolygon([(0, 0), (1, 0), (1, 1), (0, 1)], closed=True, fill=False,
                            edgecolor="#98a0ad", linewidth=1.0, linestyle="--"))
    ax.set_xlim(-0.03, 1.03)
    ax.set_ylim(-0.03, 1.03)
    ax.set_aspect("equal")
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_title(title, fontsize=9.5, color="#12233a")


def draw_map(ax, arr, title):
    a = np.asarray(arr, dtype=float)
    if a.ndim == 3 and a.shape[2] >= 3:
        ax.imshow(np.clip(a[..., :3], 0.0, 1.0))
    else:
        ax.imshow(a.reshape(a.shape[0], a.shape[1]), cmap="gray")
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_title(title, fontsize=9.0, color="#12233a", pad=3)


def deviation(before, after):
    """Per-vertex distance between two position arrays of the same mesh."""
    return np.linalg.norm(after - before, axis=1)


def heat(vmax):
    """One colour scale shared by the sculpt delta and the conform delta, so the
    two panels are directly comparable: they measure the same thing."""
    # Light at zero so the unchanged surface still reads as geometry, hot where
    # it moved.
    return cm.ScalarMappable(norm=mcolors.Normalize(0.0, vmax or 1.0), cmap="YlOrRd")


def face_heat(mappable, dev, faces):
    return [mappable.to_rgba(float(np.mean(dev[f]))) for f in faces]


def handoff_positions(mesh, verts):
    """Engine-side vertex positions, indexable by the producer's triangles.

    The handoff reader welds nothing, so the two agree; if that ever changes,
    fall back to the producer's own positions rather than draw garbage."""
    if mesh.vertex_count == len(verts):
        return mesh.positions.astype(float)
    return np.array([v[0] for v in verts], dtype=float)


def build_figure(state, path):
    """The pipeline, left to right, one column per stage."""
    fig = plt.figure(figsize=(20.5, 5.2), dpi=110)
    gs = GridSpec(2, 7, figure=fig, wspace=0.13, hspace=0.30,
                  left=0.008, right=0.975, top=0.79, bottom=0.02)

    def column(idx):
        return fig.add_subplot(gs[:, idx], projection="3d")

    info, info2 = state["info"], state["info2"]
    draw_3d(column(0), state["target_pos"], state["target_faces"], "#c0703a",
            f"1 · sculpt handoff v{info.version[0]}.{info.version[1]}\n"
            f"{info.vertex_count} verts · {info.face_count} tris\n"
            f"producer {info.producer.split('/')[-1]}")

    quality = state["quality"]
    draw_3d(column(1), state["low_pos"], state["low_faces"], "#5b9bd5",
            f"2 · retopo (remesh)\n{state['quads']} quads · {state['low_verts']} verts\n"
            f"median angle {quality['median']:.0f}°")

    atlas = state["atlas"]
    ax_uv = fig.add_subplot(gs[:, 2])
    draw_atlas(ax_uv, state["uvs"], state["faces_vt"], state["chart_colors"],
               f"3 · UV atlas (unwrap_atlas)\n{atlas.chart_count} charts · rms distortion "
               f"{atlas.rms_angle_distortion:.3f}\ncoverage {atlas.packed_area * 100:.0f}%")

    draw_map(fig.add_subplot(gs[0, 3]), state["normal"], "4 · bake — normal")
    draw_map(fig.add_subplot(gs[1, 3]), state["ao"], "bake — ambient occlusion")
    draw_map(fig.add_subplot(gs[0, 4]), state["field_normal"], "5 · bake_field — normal")
    draw_map(fig.add_subplot(gs[1, 4]), state["field_ao"],
             "bake_field — occlusion\n(Python SDF, no Target mesh)")

    # Panels 6 and 7 share one heat scale: how far the SCULPT moved, then how
    # far conform had to move the retopology to follow it.
    sculpt_dev, conform_dev = state["sculpt_dev"], state["conform_dev"]
    mappable = heat(max(float(sculpt_dev.max()), float(conform_dev.max())))
    draw_3d(column(5), state["target2_pos"], state["target_faces"],
            face_heat(mappable, sculpt_dev, state["target_faces"]),
            f"6 · the artist sculpts on\nsecond handoff v{info2.version[0]}.{info2.version[1]}"
            f" — one lump added\nsculpt moved up to {sculpt_dev.max():.2f} (retopo now stale)")

    report = state["report"]
    ax_conf = column(6)
    draw_3d(ax_conf, state["conf_pos"], state["low_faces"],
            face_heat(mappable, conform_dev, state["low_faces"]),
            f"7 · conform (same quads, same UVs)\n{report.moved_vertices} verts re-snapped · "
            f"max {report.max_deviation:.3f}\nrms {report.rms_deviation:.3f} · "
            f"{len(report.flagged)} over threshold")
    bar = fig.colorbar(mappable, ax=ax_conf, fraction=0.035, pad=0.0, shrink=0.6)
    bar.set_label("distance moved (world units)", fontsize=8)
    bar.ax.tick_params(labelsize=7)

    fig.suptitle("Sculpt handoff bridge — handoff → retopo → UV → bake, then conform "
                 "when the sculpt moves", fontsize=15, fontweight="bold",
                 color="#12233a", y=0.975)
    fig.text(0.5, 0.905, "every step through the Python bindings: Mesh.load_handoff · "
             "remesh · unwrap_atlas · bake / bake_field · conform — conform keeps the quad "
             "layout and the UVs, so the baked maps stay valid",
             fontsize=10, color="#54607a", ha="center")
    _pipeline_arrows(fig, gs)
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def _pipeline_arrows(fig, gs):
    """A '→' in the gutter between consecutive stages, so it reads as a flow."""
    _, _, lefts, rights = gs.get_grid_positions(fig)
    for col in range(len(lefts) - 1):
        fig.text((rights[col] + lefts[col + 1]) / 2.0, 0.42, "→", fontsize=20,
                 color="#8a93a3", ha="center", va="center")


def _blob_radius(p):
    """The blob's analytic surface radius in the direction of `p`."""
    n = math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2) + 1e-9
    theta = math.acos(max(-1.0, min(1.0, p[2] / n)))
    phi = math.atan2(p[1], p[0])
    return 1.0 + 0.12 * math.sin(5.0 * phi) * math.sin(4.0 * theta)


def make_field(FieldEvaluator):
    """A FieldEvaluator for the blob, as pure arithmetic — no mesh, no BVH.

    Built here rather than at module scope because the base class comes from the
    engine binding, which is only imported once the engine is known to load."""

    class AnalyticBlob(FieldEvaluator):
        def distance(self, p):
            # Scaled by 0.5 to stay a Lipschitz-<=1 LOWER bound on the true
            # distance, which is what the bake's sphere tracer requires.
            return 0.5 * (math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2) - _blob_radius(p))

        def gradient(self, p):
            n = math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2) + 1e-9
            return (p[0] / n, p[1] / n, p[2] / n)

        def occlusion(self, p, n, radius):
            # Openness in [0,1]: the blob occludes itself in its analytic
            # valleys, so the local radius is the AO term.
            return max(0.1, min(1.0, 0.55 + 1.8 * (_blob_radius(p) - 1.0)))

    return AnalyticBlob()


def run_pipeline(tmp, verts, tris):
    """Handoff -> retopo -> UV -> bake -> (sculpt moves) -> conform, all through
    the Python bindings. Returns everything the figure needs."""
    from cyberremesh import (AtlasParams, BakeMap, BakeParams, FieldEvaluator, Mesh,
                             RemeshParams, bake, bake_field, conform, remesh)

    state = {}
    handoff = emit_handoff(os.path.join(tmp, "sculpt.ply"), verts, tris)
    print(f"producer wrote {handoff} ({len(verts)} vertices, {len(tris)} triangles)\n")

    # 1. The handoff becomes a Target. The version gate runs before any geometry.
    target, info = Mesh.load_handoff(handoff)
    show_handoff("Mesh.load_handoff(sculpt.ply)", info)
    state["info"] = info
    # The engine reads one vertex per producer vertex, so the producer's own
    # triangles index the positions the engine actually loaded.
    state["target_pos"] = handoff_positions(target, verts)
    state["target_faces"] = [list(t) for t in tris]

    # 2. Retopologise the Target.
    low = remesh(target, RemeshParams(target_quad_count=500, pure_quads=True))
    state["quads"] = low.stats.quads
    state["low_verts"] = low.vertex_count
    print(f"\n  remesh           {low.stats.quads} quads / {low.stats.triangles} tris "
          f"from {info.face_count} handoff tris")

    # 3. UVs, in place — the bake needs them, and conform must not disturb them.
    atlas = low.unwrap_atlas(AtlasParams(max_chart_angle_degrees=40.0))
    state["atlas"] = atlas
    print(f"  unwrap_atlas     {atlas.chart_count} charts · rms distortion "
          f"{atlas.rms_angle_distortion:.4f} · coverage {atlas.packed_area * 100:.1f}%")

    retopo_obj = os.path.join(tmp, "retopo.obj")
    low.save_obj(retopo_obj)
    pos, uvs, faces_v, faces_vt = load_obj_uv(retopo_obj)
    cids, _ = uv_charts(uvs, faces_vt)
    cmap = plt.get_cmap("tab20")
    state.update(low_pos=pos, low_faces=faces_v, uvs=uvs, faces_vt=faces_vt,
                 chart_colors=[cmap(i % 20) for i in cids],
                 quality=common.quad_quality({"positions": pos, "faces": faces_v}))

    # 4. Bake the handoff's detail down onto the retopology's UVs.
    params = BakeParams(width=192, height=192, cage_distance=0.3, ao_samples=16, ao_radius=0.5)
    for key, kind, label in (("normal", BakeMap.NORMAL, "normal"), ("ao", BakeMap.AO, "ao")):
        img = bake(low, target, kind, params)
        state[key] = img.to_numpy()
        img.save_png(os.path.join(common.OUTPUT_DIR, f"18_bake_{label}.png"))
        print(f"  bake {label:<12} {img.width}x{img.height} x{img.channels}")
        img.close()

    # 5. The same two maps with no Target mesh at all: a Python field evaluator
    #    answers every sample instead (FieldEvaluator + bake_field).
    field = make_field(FieldEvaluator)
    for key, kind, label in (("field_normal", BakeMap.NORMAL, "normal"),
                             ("field_ao", BakeMap.AO, "ao")):
        img = bake_field(low, kind, field, params)
        state[key] = img.to_numpy()
        print(f"  bake_field {label:<6} {img.width}x{img.height} x{img.channels} "
              f"(sampled from Python, no Target mesh)")
        img.close()

    # 6. The sculpt moves on: a new handoff of the SAME asset, one lump bigger.
    verts2, tris2 = blob(bulge=BULGE_HEIGHT)
    handoff2 = emit_handoff(os.path.join(tmp, "sculpt_v2.ply"), verts2, tris2)
    target2, info2 = Mesh.load_handoff(handoff2)
    state["info2"] = info2
    state["target2_pos"] = handoff_positions(target2, verts2)
    state["sculpt_dev"] = deviation(state["target_pos"], state["target2_pos"])
    print()
    show_handoff("Mesh.load_handoff(sculpt_v2.ply)  [the artist kept sculpting]", info2)

    # 7. conform: re-snap the retopology onto the new sculpt. Topology, quad
    #    layout and UVs are untouched; only positions move.
    before = pos.copy()
    report = conform(low, target2, threshold=CONFORM_THRESHOLD)
    conf_obj = os.path.join(tmp, "conformed.obj")
    low.save_obj(conf_obj)
    after, uvs_after, conf_faces, _ = load_obj_uv(conf_obj)
    uvs_kept = uvs_after.shape == uvs.shape and bool(np.allclose(uvs_after, uvs))
    state["conf_pos"] = after
    state["report"] = report
    state["conform_dev"] = deviation(before, after)
    print(f"\n  conform          {report.moved_vertices} verts re-snapped · "
          f"max deviation {report.max_deviation:.4f} · rms {report.rms_deviation:.4f}")
    print(f"                   {len(report.flagged)} vertices over the "
          f"{CONFORM_THRESHOLD} threshold · "
          f"quads {len(conf_faces)} (unchanged) · UVs preserved {uvs_kept}")

    target.close()
    target2.close()
    low.close()
    return state


def show_version_gate(tmp, verts, tris):
    """A handoff from a future producer is rejected before any geometry is read."""
    from cyberremesh import IncompatibleVersionError, Mesh

    future = emit_handoff(os.path.join(tmp, "future.ply"), verts, tris,
                          major=HANDOFF_MAJOR + 1, minor=0)
    try:
        Mesh.load_handoff(future)
    except IncompatibleVersionError as exc:
        print(f"\n  version gate     v{HANDOFF_MAJOR + 1}.0 handoff rejected: {exc}")
        return
    print("\n  version gate     UNEXPECTED: a future handoff was accepted", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit", metavar="PATH",
                        help="write a handoff and exit ('-' for stdout)")
    args = parser.parse_args()

    verts, tris = blob()
    if args.emit:
        if args.emit == "-":
            write_handoff(sys.stdout, verts, tris)
        else:
            emit_handoff(args.emit, verts, tris)
            print(f"wrote {args.emit} ({len(verts)} vertices, {len(tris)} triangles)",
                  file=sys.stderr)
        return 0

    if common is None:
        print("the pipeline demo needs numpy + matplotlib "
              "(pip install -r examples/requirements.txt)", file=sys.stderr)
        return 1
    common.require_engine()

    with tempfile.TemporaryDirectory(prefix="cyber_handoff_demo_") as tmp:
        state = run_pipeline(tmp, verts, tris)
        show_version_gate(tmp, verts, tris)

    out = os.path.join(common.OUTPUT_DIR, "18_sculpt_handoff.png")
    build_figure(state, out)
    print(f"  wrote {os.path.relpath(out, common._REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
