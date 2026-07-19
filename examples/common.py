"""Shared helpers for the CyberRemesher Python-binding examples.

Every example drives the ``cyberremesh`` ctypes binding (over the C ABI) to run
the remeshing pipeline, then renders the input and output meshes to PNG with
matplotlib. Faces are coloured by arity so quad-dominance is visible at a
glance (blue = quad, orange = triangle, grey = n-gon).

Run examples through ``examples/run.sh`` (it locates the built shared library
and fixes the loader path); it also sets ``CYBER_CAPI_LIB``.
"""

from __future__ import annotations

import math
import os
import sys
import tempfile
import urllib.request
from typing import Dict, List, Tuple

import matplotlib

matplotlib.use("Agg")  # headless PNG rendering
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402

# Make the cyberremesh package importable without installing it.
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO, "python", "cyberremesh"))

import cyberremesh  # noqa: E402
from cyberremesh import Mesh, RemeshParams, remesh  # noqa: E402

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")
MODELS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")

Vec3 = Tuple[float, float, float]
MeshData = Dict[str, object]  # {"positions": np.ndarray (N,3), "faces": List[List[int]]}

# A curated subset of Alec Jacobson's community "common-3d-test-models"
# (https://github.com/alecjacobson/common-3d-test-models, individual model
# licenses vary — see that repo). Chosen for varied topology and features while
# staying modest in size for quick demos. Downloaded on demand into MODELS_DIR.
COMMON_3D_MODELS_BASE = (
    "https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/master/data/"
)
COMMON_3D_MODELS: Dict[str, str] = {
    "spot": "spot.obj",                # smooth organic, genus-0 (the classic cow)
    "cow": "cow.obj",                  # organic, coarse
    "fandisk": "fandisk.obj",          # CAD part with sharp creases (feature test)
    "rocker-arm": "rocker-arm.obj",    # mechanical, higher genus (handles/holes)
    "cheburashka": "cheburashka.obj",  # organic character
    "stanford-bunny": "stanford-bunny.obj",  # the iconic scanned bunny
}


def quadriflow_binary() -> "str | None":
    """Build (once, cached) and return the path to the QuadriFlow reference
    binary, or None if it cannot be built (offline / no Eigen / no toolchain).
    QuadriFlow is a field-based quad remesher standing in for AutoRemesher (which
    is GUI-only). See reference/build_quadriflow.sh."""
    import subprocess
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reference",
                          "build_quadriflow.sh")
    try:
        out = subprocess.run(["bash", script], capture_output=True, text=True, timeout=900)
        lines = [ln for ln in out.stdout.strip().splitlines() if ln.strip()]
        path = lines[-1] if lines else ""
        return path if path and os.path.exists(path) else None
    except Exception:  # noqa: BLE001 - any build/network failure -> unavailable
        return None


def quadriflow_remesh(binary: str, model_path: str, faces: int) -> MeshData:
    """Remesh `model_path` to ~`faces` quads with the QuadriFlow reference."""
    import subprocess
    with tempfile.NamedTemporaryFile(suffix=".obj", delete=False) as tmp:
        out = tmp.name
    subprocess.run([binary, "-i", model_path, "-o", out, "-f", str(faces)],
                   check=True, capture_output=True, timeout=300)
    data = load_obj(out)
    os.unlink(out)
    return data


def quad_quality(mesh: MeshData) -> "Tuple[float, float]":
    """(worst quad interior angle in degrees — 90 is ideal, higher=better;
    edge-length coefficient of variation — lower=more uniform) over the quads."""
    P = mesh["positions"]
    worst = 90.0
    lengths: List[float] = []
    for f in mesh["faces"]:
        if len(f) != 4:
            continue
        for k in range(4):
            a, b, prev = P[f[k]], P[f[(k + 1) % 4]], P[f[(k + 3) % 4]]
            e1, e2 = b - a, prev - a
            lengths.append(float(np.linalg.norm(e1)))
            c = float(np.dot(e1, e2) / (np.linalg.norm(e1) * np.linalg.norm(e2) + 1e-12))
            ang = math.degrees(math.acos(max(-1.0, min(1.0, c))))
            worst = min(worst, ang)
    if not lengths:
        return (0.0, 0.0)
    arr = np.array(lengths)
    cv = float(arr.std() / (arr.mean() + 1e-12))
    return (worst, cv)


def download_model(name: str, models_dir: str = MODELS_DIR) -> str:
    """Return a local path to a common-3d-test-model, downloading it (once) on
    demand. Raises KeyError for an unknown name."""
    if name not in COMMON_3D_MODELS:
        raise KeyError(f"unknown model {name!r}; known: {sorted(COMMON_3D_MODELS)}")
    os.makedirs(models_dir, exist_ok=True)
    path = os.path.join(models_dir, COMMON_3D_MODELS[name])
    if not os.path.exists(path):
        url = COMMON_3D_MODELS_BASE + COMMON_3D_MODELS[name]
        print(f"  downloading {name} -> {os.path.relpath(path, _REPO)}")
        urllib.request.urlretrieve(url, path)  # noqa: S310 - fixed, trusted host
    return path


# ---------------------------------------------------------------------------
# Availability
# ---------------------------------------------------------------------------
def require_engine() -> None:
    """Exit early with a helpful message if the shared library is not loadable."""
    if not cyberremesh.is_available():
        print(
            "cyber_capi shared library not loadable.\n"
            "Build it and run through the wrapper:\n"
            "  cmake --build --preset cpu-headless --target cyber_capi_shared\n"
            "  examples/run.sh examples/<script>.py",
            file=sys.stderr,
        )
        sys.exit(1)
    os.makedirs(OUTPUT_DIR, exist_ok=True)


# ---------------------------------------------------------------------------
# Procedural input meshes (written as OBJ so the binding can load them)
# ---------------------------------------------------------------------------
def _write_obj(path: str, verts: List[Vec3], faces: List[List[int]]) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        for v in verts:
            fh.write(f"v {v[0]} {v[1]} {v[2]}\n")
        for f in faces:
            fh.write("f " + " ".join(str(i + 1) for i in f) + "\n")


def sphere_obj(path: str, rings: int = 16, segments: int = 24, radius: float = 1.0) -> None:
    """Triangulated UV sphere."""
    verts: List[Vec3] = [(0.0, 0.0, radius)]
    for r in range(1, rings):
        phi = math.pi * r / rings
        for s in range(segments):
            theta = 2.0 * math.pi * s / segments
            verts.append(
                (
                    math.sin(phi) * math.cos(theta) * radius,
                    math.sin(phi) * math.sin(theta) * radius,
                    math.cos(phi) * radius,
                )
            )
    south = len(verts)
    verts.append((0.0, 0.0, -radius))

    def ring(r: int, s: int) -> int:
        return 1 + (r - 1) * segments + (s % segments)

    faces: List[List[int]] = []
    for s in range(segments):
        faces.append([0, ring(1, s), ring(1, s + 1)])
    for r in range(1, rings - 1):
        for s in range(segments):
            faces.append([ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)])
            faces.append([ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)])
    for s in range(segments):
        faces.append([south, ring(rings - 1, s + 1), ring(rings - 1, s)])
    _write_obj(path, verts, [[i for i in f] for f in faces])


def torus_obj(path: str, major: int = 40, minor: int = 20, r_major: float = 1.0,
              r_minor: float = 0.4) -> None:
    """Triangulated torus (genus 1)."""
    verts: List[Vec3] = []
    for i in range(major):
        u = 2.0 * math.pi * i / major
        for j in range(minor):
            v = 2.0 * math.pi * j / minor
            cu, su, cv, sv = math.cos(u), math.sin(u), math.cos(v), math.sin(v)
            verts.append(((r_major + r_minor * cv) * cu, (r_major + r_minor * cv) * su, r_minor * sv))

    def idx(i: int, j: int) -> int:
        return (i % major) * minor + (j % minor)

    faces: List[List[int]] = []
    for i in range(major):
        for j in range(minor):
            faces.append([idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)])
            faces.append([idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)])
    _write_obj(path, verts, faces)


def cube_obj(path: str, subdiv: int = 10) -> None:
    """A subdivided cube (sharp 90-degree edges) as triangles."""
    verts: List[Vec3] = []
    faces: List[List[int]] = []

    def add_face(origin: Vec3, du: Vec3, dv: Vec3) -> None:
        base = len(verts)
        for a in range(subdiv + 1):
            for b in range(subdiv + 1):
                fa, fb = a / subdiv, b / subdiv
                verts.append(
                    (
                        origin[0] + du[0] * fa + dv[0] * fb,
                        origin[1] + du[1] * fa + dv[1] * fb,
                        origin[2] + du[2] * fa + dv[2] * fb,
                    )
                )
        n = subdiv + 1
        for a in range(subdiv):
            for b in range(subdiv):
                i0 = base + a * n + b
                faces.append([i0, i0 + 1, i0 + n + 1])
                faces.append([i0, i0 + n + 1, i0 + n])

    add_face((0, 0, 0), (1, 0, 0), (0, 1, 0))
    add_face((0, 0, 1), (0, 1, 0), (1, 0, 0))
    add_face((0, 0, 0), (0, 1, 0), (0, 0, 1))
    add_face((1, 0, 0), (0, 0, 1), (0, 1, 0))
    add_face((0, 0, 0), (0, 0, 1), (1, 0, 0))
    add_face((0, 1, 0), (1, 0, 0), (0, 0, 1))
    _write_obj(path, verts, faces)


def bumpy_sphere_obj(path: str, rings: int = 32, segments: int = 48) -> None:
    """A sphere with high-frequency bumps near the poles — curvature varies,
    so adaptivity has visible work to do."""
    verts: List[Vec3] = []
    faces: List[List[int]] = []
    for r in range(rings + 1):
        phi = math.pi * r / rings
        bump = 1.0 + 0.18 * math.sin(6.0 * phi) ** 2
        for s in range(segments):
            theta = 2.0 * math.pi * s / segments
            rad = bump
            verts.append(
                (
                    math.sin(phi) * math.cos(theta) * rad,
                    math.sin(phi) * math.sin(theta) * rad,
                    math.cos(phi) * rad,
                )
            )

    def idx(r: int, s: int) -> int:
        return r * segments + (s % segments)

    for r in range(rings):
        for s in range(segments):
            faces.append([idx(r, s), idx(r + 1, s), idx(r + 1, s + 1)])
            faces.append([idx(r, s), idx(r + 1, s + 1), idx(r, s + 1)])
    _write_obj(path, verts, faces)


def uv_sphere_obj(path: str, rings: int = 24, segments: int = 48, radius: float = 1.0,
                  bump: float = 0.0) -> None:
    """A UV sphere as a quad grid with per-corner UVs (vt) — needed as the
    low-poly bake target. `bump` adds a dimpled displacement (use it for the
    high-poly Target so the baked maps have detail to capture)."""
    verts: List[Vec3] = []
    uvs: List[Tuple[float, float]] = []
    for r in range(rings + 1):
        phi = math.pi * r / rings
        v = r / rings
        for s in range(segments + 1):  # duplicate seam column for clean UVs
            theta = 2.0 * math.pi * s / segments
            # Outward-only dimples (0..bump): the smooth low-poly (bump=0) is the
            # base and detail rises above it, so baked AO darkens in the valleys
            # rather than flipping hard where the surface pokes through.
            disp = bump * 0.5 * (1.0 + math.sin(7.0 * phi) * math.cos(9.0 * theta))
            rad = radius + disp
            verts.append(
                (math.sin(phi) * math.cos(theta) * rad, math.sin(phi) * math.sin(theta) * rad,
                 math.cos(phi) * rad)
            )
            uvs.append((s / segments, v))

    def idx(r: int, s: int) -> int:
        return r * (segments + 1) + s

    faces: List[List[int]] = []
    for r in range(rings):
        for s in range(segments):
            faces.append([idx(r, s), idx(r + 1, s), idx(r + 1, s + 1), idx(r, s + 1)])

    with open(path, "w", encoding="utf-8") as fh:
        for p in verts:
            fh.write(f"v {p[0]} {p[1]} {p[2]}\n")
        for t in uvs:
            fh.write(f"vt {t[0]} {t[1]}\n")
        for f in faces:
            fh.write("f " + " ".join(f"{i + 1}/{i + 1}" for i in f) + "\n")


def torus_knot_obj(path: str, p: int = 3, q: int = 2, curve_segments: int = 200,
                   tube_segments: int = 16, tube_radius: float = 0.22) -> None:
    """A triangulated (p, q) torus-knot tube — a non-trivial 'model' to convert
    to quads. The tube frame is parallel-transported so it stays untwisted."""
    n = curve_segments
    curve = np.zeros((n, 3))
    for i in range(n):
        t = 2.0 * math.pi * i / n
        r = 2.0 + math.cos(q * t)
        curve[i] = [r * math.cos(p * t), r * math.sin(p * t), -math.sin(q * t)]

    tangents = np.zeros((n, 3))
    for i in range(n):
        tangents[i] = curve[(i + 1) % n] - curve[(i - 1) % n]
        tangents[i] /= np.linalg.norm(tangents[i]) + 1e-12

    ref = np.array([0.0, 0.0, 1.0])
    first = np.cross(tangents[0], ref)
    if np.linalg.norm(first) < 1e-6:
        first = np.cross(tangents[0], np.array([0.0, 1.0, 0.0]))
    normals = [first / np.linalg.norm(first)]
    for i in range(1, n):  # parallel transport
        v = normals[-1] - tangents[i] * float(np.dot(normals[-1], tangents[i]))
        norm = np.linalg.norm(v)
        normals.append(v / norm if norm > 1e-9 else normals[-1])
    normals_arr = np.array(normals)
    binormals = np.cross(tangents, normals_arr)

    verts: List[Vec3] = []
    for i in range(n):
        for j in range(tube_segments):
            a = 2.0 * math.pi * j / tube_segments
            offset = (normals_arr[i] * math.cos(a) + binormals[i] * math.sin(a)) * tube_radius
            pt = curve[i] + offset
            verts.append((float(pt[0]), float(pt[1]), float(pt[2])))

    def idx(i: int, j: int) -> int:
        return (i % n) * tube_segments + (j % tube_segments)

    faces: List[List[int]] = []
    for i in range(n):
        for j in range(tube_segments):
            a, b, c, d = idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)
            faces.append([a, b, c])  # triangulated so the demo is a real tri->quad conversion
            faces.append([a, c, d])
    _write_obj(path, verts, faces)


def load_any(path: str) -> MeshData:
    """Load any engine-supported mesh (OBJ / PLY / STL / glTF) for rendering by
    routing it through the engine (load -> export OBJ -> parse)."""
    with Mesh.load_obj(path) as mesh:
        with tempfile.NamedTemporaryFile(suffix=".obj", delete=False) as tmp:
            out = tmp.name
        mesh.save_obj(out)
    data = load_obj(out)
    os.unlink(out)
    return data


def plane_with_hole_obj(path: str, n: int = 12) -> None:
    """A flat grid with a square hole punched in the middle (for hole-fill)."""
    verts: List[Vec3] = [
        (float(i) / n - 0.5, float(j) / n - 0.5, 0.0) for i in range(n + 1) for j in range(n + 1)
    ]

    def idx(i: int, j: int) -> int:
        return i * (n + 1) + j

    hole_lo, hole_hi = n // 2 - 1, n // 2 + 1
    faces: List[List[int]] = []
    for i in range(n):
        for j in range(n):
            if hole_lo <= i < hole_hi and hole_lo <= j < hole_hi:
                continue  # leave a hole
            faces.append([idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)])
    _write_obj(path, verts, faces)


# ---------------------------------------------------------------------------
# OBJ parsing (read remesh output back for rendering)
# ---------------------------------------------------------------------------
def load_obj(path: str) -> MeshData:
    positions: List[Vec3] = []
    faces: List[List[int]] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "v" and len(parts) >= 4:
                positions.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif parts[0] == "f":
                faces.append([int(p.split("/")[0]) - 1 for p in parts[1:]])
    return {"positions": np.array(positions, dtype=float), "faces": faces}


# ---------------------------------------------------------------------------
# Pipeline convenience
# ---------------------------------------------------------------------------
def remesh_obj(input_obj: str, params: RemeshParams) -> Tuple[MeshData, "cyberremesh.Statistics"]:
    """Remesh an OBJ and return (parsed output mesh, statistics)."""
    with Mesh.load_obj(input_obj) as src:
        result = remesh(src, params)
        with tempfile.NamedTemporaryFile(suffix=".obj", delete=False) as tmp:
            out_path = tmp.name
        result.save_obj(out_path)
        stats = result.stats
        result.close()
    mesh = load_obj(out_path)
    os.unlink(out_path)
    return mesh, stats


def face_counts(mesh: MeshData) -> Tuple[int, int, int]:
    """(quads, triangles, other) in a parsed mesh."""
    q = t = o = 0
    for f in mesh["faces"]:  # type: ignore[index]
        n = len(f)
        q += n == 4
        t += n == 3
        o += n not in (3, 4)
    return q, t, o


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
_QUAD = "#5b9bd5"
_TRI = "#ed9b40"
_OTHER = "#9aa0a6"
_EDGE = "#20262e"


def _draw(ax, mesh: MeshData, title: str) -> None:
    pos = mesh["positions"]  # type: ignore[index]
    faces = mesh["faces"]  # type: ignore[index]
    polys, colors = [], []
    for f in faces:
        polys.append([pos[i] for i in f])
        n = len(f)
        colors.append(_QUAD if n == 4 else _TRI if n == 3 else _OTHER)
    coll = Poly3DCollection(polys, facecolors=colors, edgecolors=_EDGE, linewidths=0.25)
    coll.set_alpha(1.0)
    ax.add_collection3d(coll)

    lo = pos.min(axis=0)
    hi = pos.max(axis=0)
    center = (lo + hi) / 2.0
    span = float((hi - lo).max()) * 0.55 or 1.0
    for setlim, c in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), center):
        setlim(c - span, c + span)
    ax.set_box_aspect((1, 1, 1))
    ax.set_axis_off()
    ax.view_init(elev=22, azim=35)
    ax.set_title(title, fontsize=11, color="#20262e", pad=2)


def render_panels(meshes: List[MeshData], titles: List[str], path: str, suptitle: str = "") -> None:
    """Render N meshes side by side into one PNG."""
    n = len(meshes)
    fig = plt.figure(figsize=(4.6 * n, 4.9), dpi=130)
    if suptitle:
        fig.suptitle(suptitle, fontsize=13, fontweight="bold", color="#12233a", y=0.98)
    for k, (mesh, title) in enumerate(zip(meshes, titles)):
        ax = fig.add_subplot(1, n, k + 1, projection="3d")
        _draw(ax, mesh, title)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(path, _REPO)}")


def render_grid(meshes: List[MeshData], titles: List[str], path: str, cols: int = 3,
                suptitle: str = "") -> None:
    """Render N meshes in a rows x cols grid into one PNG (for galleries)."""
    n = len(meshes)
    cols = max(1, min(cols, n))
    rows = (n + cols - 1) // cols
    fig = plt.figure(figsize=(4.6 * cols, 4.7 * rows), dpi=120)
    if suptitle:
        fig.suptitle(suptitle, fontsize=14, fontweight="bold", color="#12233a", y=0.99)
    for k, (mesh, title) in enumerate(zip(meshes, titles)):
        ax = fig.add_subplot(rows, cols, k + 1, projection="3d")
        _draw(ax, mesh, title)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(path, _REPO)}")


def render_maps(maps: List[Tuple[str, "np.ndarray"]], path: str, suptitle: str = "") -> None:
    """Render baked maps as image panels. RGB maps show directly; single-channel
    maps (AO, displacement) are min-max normalised to greyscale."""
    n = len(maps)
    fig, axes = plt.subplots(1, n, figsize=(3.7 * n, 4.0), dpi=130)
    if n == 1:
        axes = [axes]
    if suptitle:
        fig.suptitle(suptitle, fontsize=13, fontweight="bold", color="#12233a")
    for ax, (title, arr) in zip(axes, maps):
        a = np.asarray(arr, dtype=float)
        if a.ndim == 3 and a.shape[2] >= 3:
            ax.imshow(np.clip(a[..., :3], 0.0, 1.0))
        else:
            g = a.reshape(a.shape[0], a.shape[1])
            lo, hi = float(g.min()), float(g.max())
            if hi > lo:
                g = (g - lo) / (hi - lo)
            ax.imshow(g, cmap="gray")
        ax.set_title(title, fontsize=11, color="#20262e")
        ax.set_xticks([])
        ax.set_yticks([])
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(path, _REPO)}")


def stat_line(name: str, stats: "cyberremesh.Statistics", mesh: MeshData) -> str:
    q, t, o = face_counts(mesh)
    total = q + t + o or 1
    return f"{name:<22} verts={stats.vertices:<6} quads={q:<6} tris={t:<5} quad%={100*q//total}"
