#!/usr/bin/env python3
"""Quad-remesh quality metrics (task 5.9).

All metrics are computed from the meshes alone (no solver internals) so the
same scoreboard applies to cyberremesh and to external baseline binaries.
Distances are sampling-based approximations — stable for trend tracking, not
certified bounds — and are normalized by the input's bounding-box diagonal.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
from scipy.spatial import cKDTree

DEFAULT_SAMPLES = 100_000
DEFAULT_SHARP_DEGREES = 44.0
FEATURE_TOL_FRACTION = 0.005  # of bbox diagonal


@dataclass
class MeshData:
    vertices: np.ndarray  # (n, 3) float64
    faces: list[tuple[int, ...]] = field(default_factory=list)

    def triangles(self) -> np.ndarray:
        """Fan-triangulate every face; returns (m, 3) int array."""
        tris = []
        for f in self.faces:
            for i in range(1, len(f) - 1):
                tris.append((f[0], f[i], f[i + 1]))
        return np.asarray(tris, dtype=np.int64).reshape(-1, 3)

    def bbox_diagonal(self) -> float:
        if len(self.vertices) == 0:
            return 0.0
        return float(np.linalg.norm(self.vertices.max(axis=0) - self.vertices.min(axis=0)))


def load_obj(path: str) -> MeshData:
    verts: list[tuple[float, float, float]] = []
    faces: list[tuple[int, ...]] = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("v "):
                parts = line.split()
                verts.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif line.startswith("f "):
                idx = []
                for token in line.split()[1:]:
                    raw = token.split("/")[0]
                    i = int(raw)
                    idx.append(i - 1 if i > 0 else len(verts) + i)
                if len(idx) >= 3:
                    faces.append(tuple(idx))
    return MeshData(np.asarray(verts, dtype=np.float64).reshape(-1, 3), faces)


def face_counts(mesh: MeshData) -> dict:
    arity: dict[int, int] = {}
    for f in mesh.faces:
        arity[len(f)] = arity.get(len(f), 0) + 1
    total = len(mesh.faces)
    quads = arity.get(4, 0)
    return {
        "vertices": int(len(mesh.vertices)),
        "faces": total,
        "quads": quads,
        "triangles": arity.get(3, 0),
        "ngons": total - quads - arity.get(3, 0),
        "quad_ratio": (quads / total) if total else 0.0,
    }


def _edge_face_counts(mesh: MeshData) -> dict[tuple[int, int], int]:
    counts: dict[tuple[int, int], int] = {}
    for f in mesh.faces:
        for a, b in zip(f, f[1:] + f[:1]):
            key = (a, b) if a < b else (b, a)
            counts[key] = counts.get(key, 0) + 1
    return counts


def singularity_stats(mesh: MeshData) -> dict:
    """Irregular-vertex count: interior vertices with valence != 4.

    Exact for pure-quad meshes; for quad-dominant output it also counts
    vertices touching leftover triangles, which is the honest reading (those
    are flow defects too).
    """
    edge_counts = _edge_face_counts(mesh)
    valence: dict[int, int] = {}
    boundary_verts: set[int] = set()
    for (a, b), n in edge_counts.items():
        valence[a] = valence.get(a, 0) + 1
        valence[b] = valence.get(b, 0) + 1
        if n == 1:
            boundary_verts.add(a)
            boundary_verts.add(b)
    irregular = sum(
        1 for v, val in valence.items() if v not in boundary_verts and val != 4
    )
    interior = sum(1 for v in valence if v not in boundary_verts)
    return {
        "singularities": irregular,
        "interior_vertices": interior,
        "singularity_ratio": (irregular / interior) if interior else 0.0,
    }


def _area_weighted_samples(mesh: MeshData, count: int, seed: int = 7) -> np.ndarray:
    tris = mesh.triangles()
    if len(tris) == 0:
        return np.zeros((0, 3))
    v = mesh.vertices
    a, b, c = v[tris[:, 0]], v[tris[:, 1]], v[tris[:, 2]]
    areas = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    total = areas.sum()
    if total <= 0.0:
        return np.zeros((0, 3))
    rng = np.random.default_rng(seed)
    chosen = rng.choice(len(tris), size=count, p=areas / total)
    r1 = np.sqrt(rng.random(count))[:, None]
    r2 = rng.random(count)[:, None]
    return (1 - r1) * a[chosen] + r1 * (1 - r2) * b[chosen] + r1 * r2 * c[chosen]


def distance_stats(reference: MeshData, result: MeshData, samples: int = DEFAULT_SAMPLES) -> dict:
    """Symmetric sampled surface distance, normalized by reference bbox diagonal."""
    diag = reference.bbox_diagonal()
    ref_pts = _area_weighted_samples(reference, samples, seed=7)
    res_pts = _area_weighted_samples(result, samples, seed=11)
    if diag <= 0.0 or len(ref_pts) == 0 or len(res_pts) == 0:
        return {"hausdorff_p99": math.inf, "chamfer_mean": math.inf}
    d_res_to_ref, _ = cKDTree(ref_pts).query(res_pts, workers=-1)
    d_ref_to_res, _ = cKDTree(res_pts).query(ref_pts, workers=-1)
    both = np.concatenate([d_res_to_ref, d_ref_to_res]) / diag
    return {
        "hausdorff_p99": float(np.quantile(both, 0.99)),
        "chamfer_mean": float(both.mean()),
    }


def quad_quality(mesh: MeshData) -> dict:
    """Corner-angle deviation from 90° and edge-length spread, quads only."""
    v = mesh.vertices
    deviations: list[float] = []
    lengths: list[float] = []
    for f in mesh.faces:
        if len(f) != 4:
            continue
        pts = v[list(f)]
        for i in range(4):
            e1 = pts[(i + 1) % 4] - pts[i]
            e2 = pts[(i - 1) % 4] - pts[i]
            n1, n2 = np.linalg.norm(e1), np.linalg.norm(e2)
            if n1 <= 0.0 or n2 <= 0.0:
                continue
            cosine = float(np.clip(e1 @ e2 / (n1 * n2), -1.0, 1.0))
            deviations.append(abs(math.degrees(math.acos(cosine)) - 90.0))
            lengths.append(float(n1))
    if not deviations:
        return {"angle_dev_mean": 180.0, "angle_dev_p95": 180.0, "edge_length_cv": math.inf}
    dev = np.asarray(deviations)
    lens = np.asarray(lengths)
    return {
        "angle_dev_mean": float(dev.mean()),
        "angle_dev_p95": float(np.quantile(dev, 0.95)),
        "edge_length_cv": float(lens.std() / lens.mean()) if lens.mean() > 0 else math.inf,
    }


def _sharp_edges(mesh: MeshData, threshold_deg: float) -> list[tuple[int, int]]:
    tris = mesh.triangles()
    v = mesh.vertices
    a, b, c = v[tris[:, 0]], v[tris[:, 1]], v[tris[:, 2]]
    normals = np.cross(b - a, c - a)
    norms = np.linalg.norm(normals, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    normals /= norms

    edge_faces: dict[tuple[int, int], list[int]] = {}
    for fi, tri in enumerate(tris):
        for x, y in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
            key = (int(x), int(y)) if x < y else (int(y), int(x))
            edge_faces.setdefault(key, []).append(fi)

    cos_threshold = math.cos(math.radians(threshold_deg))
    sharp = []
    for edge, incident in edge_faces.items():
        if len(incident) != 2:
            continue
        if float(normals[incident[0]] @ normals[incident[1]]) < cos_threshold:
            sharp.append(edge)
    return sharp


def _sample_segments(v: np.ndarray, edges: list[tuple[int, int]], step: float) -> np.ndarray:
    pts = []
    for a, b in edges:
        pa, pb = v[a], v[b]
        n = max(2, int(np.linalg.norm(pb - pa) / step) + 1)
        t = np.linspace(0.0, 1.0, n)[:, None]
        pts.append((1 - t) * pa + t * pb)
    return np.concatenate(pts) if pts else np.zeros((0, 3))


def feature_recall(reference: MeshData, result: MeshData,
                   sharp_degrees: float = DEFAULT_SHARP_DEGREES) -> dict:
    """Fraction of the input's sharp-edge length reproduced by output edges."""
    diag = reference.bbox_diagonal()
    sharp = _sharp_edges(reference, sharp_degrees)
    if not sharp or diag <= 0.0:
        return {"feature_recall": None, "sharp_edge_count": 0}
    tol = FEATURE_TOL_FRACTION * diag
    ref_pts = _sample_segments(reference.vertices, sharp, step=tol)
    out_edges = list(_edge_face_counts(result).keys())
    out_pts = _sample_segments(result.vertices, out_edges, step=tol)
    if len(out_pts) == 0:
        return {"feature_recall": 0.0, "sharp_edge_count": len(sharp)}
    d, _ = cKDTree(out_pts).query(ref_pts, workers=-1)
    return {
        "feature_recall": float((d <= tol).mean()),
        "sharp_edge_count": len(sharp),
    }


def _quad_half_edges(mesh: MeshData) -> dict[tuple[int, int], tuple[tuple[int, ...], int]]:
    """Directed half-edge -> (quad face, corner index), quad faces only.

    Half-edges shared by more than one quad (non-manifold) are dropped so
    loop tracing terminates there instead of following an arbitrary face.
    """
    table: dict[tuple[int, int], tuple[tuple[int, ...], int]] = {}
    duplicates: set[tuple[int, int]] = set()
    for f in mesh.faces:
        if len(f) != 4:
            continue
        for i in range(4):
            he = (f[i], f[(i + 1) % 4])
            if he in table:
                duplicates.add(he)
            else:
                table[he] = (f, i)
    for he in duplicates:
        del table[he]
    return table


def _opposite_half_edge(entry: tuple[tuple[int, ...], int]) -> tuple[int, int]:
    """The quad's edge opposite the entry half-edge, in face winding order."""
    f, i = entry
    return (f[(i + 2) % 4], f[(i + 3) % 4])


def _rewind(he: tuple[int, int], table: dict) -> tuple[int, int]:
    """Walk backward to the loop's first half-edge (returns `he` if closed)."""
    start = he
    for _ in range(len(table)):
        entry = table.get((start[1], start[0]))
        if entry is None:  # boundary or non-quad neighbor: open loop starts here
            return start
        prev = _opposite_half_edge(entry)
        if prev == he:  # came back around: closed loop, any start works
            return he
        if prev not in table:  # neighbor's half-edge dropped (non-manifold)
            return start
        start = prev
    return start


def _trace_loop(start: tuple[int, int], table: dict, visited: set,
                v: np.ndarray) -> tuple[np.ndarray, int, bool]:
    """Cross quads via opposite edges from `start`; returns (midpoint polyline,
    steps, closed). Marks both directions of every crossing as visited."""
    points = [(v[start[0]] + v[start[1]]) * 0.5]
    he = start
    steps = 0
    closed = False
    for _ in range(len(table)):
        if he not in table:  # half-edge dropped as non-manifold: loop ends
            break
        visited.add(he)
        opposite = _opposite_half_edge(table[he])
        visited.add(opposite)  # same crossing traversed in reverse
        points.append((v[opposite[0]] + v[opposite[1]]) * 0.5)
        steps += 1
        nxt = (opposite[1], opposite[0])
        if nxt == start:
            closed = True
            break
        if nxt not in table or nxt in visited:
            break  # left the all-quad region, hit a boundary, or a traced line
        he = nxt
    return np.asarray(points), steps, closed


def _turning_angles(points: np.ndarray, closed: bool) -> list[float]:
    """Absolute turning angles (degrees) between consecutive loop segments."""
    segments = np.diff(points, axis=0)
    norms = np.linalg.norm(segments, axis=1)
    keep = norms > 0.0
    segments, norms = segments[keep], norms[keep]
    if len(segments) < 2:
        return []
    d = segments / norms[:, None]
    a, b = (d, np.roll(d, -1, axis=0)) if closed else (d[:-1], d[1:])
    cosines = np.clip(np.einsum("ij,ij->i", a, b), -1.0, 1.0)
    return list(np.degrees(np.arccos(cosines)))


def flow_stats(mesh: MeshData) -> dict:
    """Edge-flow coherence from quad edge loops (opposite-edge continuation).

    Loops are polylines through quad edge midpoints; a loop ends at a boundary,
    when it leaves the all-quad region, or when it returns to its start. Some
    turning is inherent on curved surfaces, so values compare solvers on the
    same input rather than serving as absolute bounds.
    """
    table = _quad_half_edges(mesh)
    if not table:
        return {"flow_turning_mean": None, "flow_loop_mean_len": None}
    visited: set[tuple[int, int]] = set()
    turning: list[float] = []
    lengths: list[int] = []
    for he in table:
        if he in visited:
            continue
        start = _rewind(he, table)
        if start in visited:
            continue
        points, steps, closed = _trace_loop(start, table, visited, mesh.vertices)
        lengths.append(steps)
        turning.extend(_turning_angles(points, closed))
    return {
        "flow_turning_mean": float(np.mean(turning)) if turning else None,
        "flow_loop_mean_len": float(np.mean(lengths)) if lengths else None,
    }


def compute_all(input_path: str, output_path: str,
                samples: int = DEFAULT_SAMPLES,
                sharp_degrees: float = DEFAULT_SHARP_DEGREES) -> dict:
    reference = load_obj(input_path)
    result = load_obj(output_path)
    metrics: dict = {}
    metrics.update(face_counts(result))
    metrics.update(singularity_stats(result))
    metrics.update(distance_stats(reference, result, samples))
    metrics.update(quad_quality(result))
    metrics.update(flow_stats(result))
    metrics.update(feature_recall(reference, result, sharp_degrees))
    return metrics
