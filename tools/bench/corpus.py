#!/usr/bin/env python3
"""Benchmark corpus (task 5.9).

Two tiers:
- *generated*: deterministic procedural meshes, built on demand — no network,
  used by the CI regression check.
- *downloaded*: real scans/sculpts from `corpus.json` (permissive licenses
  only; each entry records url, license, sha256). Used for local competitive
  benchmarking against external solvers.
"""

from __future__ import annotations

import hashlib
import json
import math
import urllib.request
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
MANIFEST = BENCH_DIR / "corpus.json"


def _write_obj(path: Path, verts: list, faces: list) -> None:
    with path.open("w") as f:
        for v in verts:
            f.write(f"v {v[0]:.9g} {v[1]:.9g} {v[2]:.9g}\n")
        for face in faces:
            f.write("f " + " ".join(str(i + 1) for i in face) + "\n")


def _sphere(rings: int = 24, segments: int = 32) -> tuple[list, list]:
    verts = [(0.0, 0.0, 1.0)]
    for r in range(1, rings):
        phi = math.pi * r / rings
        for s in range(segments):
            theta = 2 * math.pi * s / segments
            verts.append((math.sin(phi) * math.cos(theta),
                          math.sin(phi) * math.sin(theta), math.cos(phi)))
    verts.append((0.0, 0.0, -1.0))
    south = len(verts) - 1

    def ring(r: int, s: int) -> int:
        return 1 + (r - 1) * segments + (s % segments)

    faces = []
    for s in range(segments):
        faces.append((0, ring(1, s), ring(1, s + 1)))
    for r in range(1, rings - 1):
        for s in range(segments):
            faces.append((ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)))
            faces.append((ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)))
    for s in range(segments):
        faces.append((south, ring(rings - 1, s + 1), ring(rings - 1, s)))
    return verts, faces


def _box(divisions: int = 12) -> tuple[list, list]:
    """Axis-aligned unit box, each face a triangulated grid — 12 sharp edges."""
    verts: list = []
    faces: list = []
    index: dict[tuple[float, float, float], int] = {}

    def vid(p: tuple[float, float, float]) -> int:
        key = (round(p[0], 9), round(p[1], 9), round(p[2], 9))
        if key not in index:
            index[key] = len(verts)
            verts.append(key)
        return index[key]

    def grid_face(origin, du, dv):
        for i in range(divisions):
            for j in range(divisions):
                def pt(a, b):
                    return tuple(origin[k] + du[k] * a / divisions + dv[k] * b / divisions
                                 for k in range(3))
                p00, p10 = vid(pt(i, j)), vid(pt(i + 1, j))
                p11, p01 = vid(pt(i + 1, j + 1)), vid(pt(i, j + 1))
                faces.append((p00, p10, p11))
                faces.append((p00, p11, p01))

    grid_face((-0.5, -0.5, 0.5), (1, 0, 0), (0, 1, 0))    # +z
    grid_face((0.5, -0.5, -0.5), (-1, 0, 0), (0, 1, 0))   # -z
    grid_face((0.5, -0.5, 0.5), (0, 0, -1), (0, 1, 0))    # +x
    grid_face((-0.5, -0.5, -0.5), (0, 0, 1), (0, 1, 0))   # -x
    grid_face((-0.5, 0.5, 0.5), (1, 0, 0), (0, 0, -1))    # +y
    grid_face((-0.5, -0.5, -0.5), (1, 0, 0), (0, 0, 1))   # -y
    return verts, faces


def _torus(major: float = 1.0, minor: float = 0.35,
           rings: int = 36, segments: int = 18) -> tuple[list, list]:
    verts = []
    for r in range(rings):
        u = 2 * math.pi * r / rings
        for s in range(segments):
            v = 2 * math.pi * s / segments
            verts.append(((major + minor * math.cos(v)) * math.cos(u),
                          (major + minor * math.cos(v)) * math.sin(u),
                          minor * math.sin(v)))
    faces = []
    for r in range(rings):
        for s in range(segments):
            a = r * segments + s
            b = ((r + 1) % rings) * segments + s
            c = ((r + 1) % rings) * segments + (s + 1) % segments
            d = r * segments + (s + 1) % segments
            faces.append((a, b, c))
            faces.append((a, c, d))
    return verts, faces


def _cylinder(segments: int = 48, height_divs: int = 12) -> tuple[list, list]:
    """Capped cylinder: smooth barrel + two sharp rims (mixed-feature case)."""
    verts = []
    for h in range(height_divs + 1):
        z = h / height_divs - 0.5
        for s in range(segments):
            t = 2 * math.pi * s / segments
            verts.append((0.5 * math.cos(t), 0.5 * math.sin(t), z))
    top_center = len(verts)
    verts.append((0.0, 0.0, 0.5))
    bottom_center = len(verts)
    verts.append((0.0, 0.0, -0.5))

    faces = []
    for h in range(height_divs):
        for s in range(segments):
            a = h * segments + s
            b = h * segments + (s + 1) % segments
            c = (h + 1) * segments + (s + 1) % segments
            d = (h + 1) * segments + s
            faces.append((a, b, c))
            faces.append((a, c, d))
    top_row = height_divs * segments
    for s in range(segments):
        faces.append((top_center, top_row + s, top_row + (s + 1) % segments))
        faces.append((bottom_center, (s + 1) % segments, s))
    return verts, faces


GENERATED = {
    "sphere": (_sphere, 800),        # name -> (builder, default target quads)
    "box_sharp": (_box, 600),
    "torus": (_torus, 700),
    "cylinder": (_cylinder, 600),
}


def generated_meshes(cache_dir: Path) -> list[dict]:
    cache_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for name, (builder, target_quads) in GENERATED.items():
        path = cache_dir / f"{name}.obj"
        if not path.exists():
            verts, faces = builder()
            _write_obj(path, verts, faces)
        entries.append({"name": name, "path": path, "target_quads": target_quads})
    return entries


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def downloaded_meshes(cache_dir: Path) -> list[dict]:
    manifest = json.loads(MANIFEST.read_text())
    cache_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for item in manifest["meshes"]:
        path = cache_dir / f"{item['name']}.obj"
        if not path.exists():
            print(f"fetching {item['name']} from {item['url']}")
            urllib.request.urlretrieve(item["url"], path)
        digest = _sha256(path)
        if item.get("sha256"):
            if digest != item["sha256"]:
                raise RuntimeError(
                    f"{item['name']}: sha256 mismatch (got {digest}); delete the "
                    f"cached file or fix corpus.json")
        else:
            print(f"note: pin {item['name']} in corpus.json with \"sha256\": \"{digest}\"")
        entries.append({"name": item["name"], "path": path,
                        "target_quads": item["target_quads"]})
    return entries
