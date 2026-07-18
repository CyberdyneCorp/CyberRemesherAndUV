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

Vec3 = Tuple[float, float, float]
MeshData = Dict[str, object]  # {"positions": np.ndarray (N,3), "faces": List[List[int]]}


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


def stat_line(name: str, stats: "cyberremesh.Statistics", mesh: MeshData) -> str:
    q, t, o = face_counts(mesh)
    total = q + t + o or 1
    return f"{name:<22} verts={stats.vertices:<6} quads={q:<6} tris={t:<5} quad%={100*q//total}"
