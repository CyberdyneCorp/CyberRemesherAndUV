#!/usr/bin/env python3
"""Open-surface cleanup keeps the trace dense and uniform (roadmap Phase 3, lever c8).

An OPEN island (a surface with a genuine boundary rim) used to under-trace
catastrophically at low target density: the `fixHoles` cleanup that fills the
under-traced gaps was opt-in, so by default an open paraboloid at a ~900-quad
request came out as ~90 huge faces at ~27-degree median angle.

The cleanup is now on by default. What kept it opt-in was an edge-length CV blowup
(0.31 -> 1.70) that a bisection traced to `simplifyGraph` -- it dissolves every
valence-2 graph node, and on an open surface most isoline samples are legitimately
valence-2, so it merges cells into long uneven quads. The fix runs `fixHoles` but
skips `simplifyGraph` (and the collapse steps that re-trigger it) on open islands.

This is the regression guard, exercised through the real remesh path on an open
paraboloid: the output must be dense (not the sparse under-traced case) and uniform
(edge-length CV well below the pre-fix blowup), while staying manifold. Closed
inputs are byte-identical and are covered by the golden tests.

Runnable as a plain script; exits 77 (CTest SKIP) if the library is absent.
"""

import math
import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

from cyberremesh import Mesh, RemeshParams, remesh

FAILURES: "list" = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def write_paraboloid(path: str, n: int = 40) -> None:
    """An open bowl z = 0.45 (x^2 + y^2) over [-1,1]^2 -- a single boundary rim."""
    lines = []
    for i in range(n + 1):
        for j in range(n + 1):
            x = i / n * 2 - 1
            y = j / n * 2 - 1
            lines.append(f"v {x:.6f} {y:.6f} {0.45 * (x * x + y * y):.6f}")
    idx = lambda i, j: i * (n + 1) + j + 1  # noqa: E731 - 1-based OBJ index
    for i in range(n):
        for j in range(n):
            lines.append(f"f {idx(i, j)} {idx(i + 1, j)} {idx(i + 1, j + 1)}")
            lines.append(f"f {idx(i, j)} {idx(i + 1, j + 1)} {idx(i, j + 1)}")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def read_obj(path: str):
    verts, faces = [], []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("v "):
                verts.append([float(t) for t in line.split()[1:4]])
            elif line.startswith("f "):
                faces.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return verts, faces


def edge_cv(verts, faces) -> float:
    lens = []
    for f in faces:
        if len(f) != 4:
            continue
        for k in range(4):
            a, b = verts[f[k]], verts[f[(k + 1) % 4]]
            lens.append(math.dist(a, b))
    if not lens:
        return float("inf")
    mean = sum(lens) / len(lens)
    var = sum((x - mean) ** 2 for x in lens) / len(lens)
    return math.sqrt(var) / mean if mean else float("inf")


def nonmanifold_edges(faces) -> int:
    use: dict = {}
    for f in faces:
        for k in range(len(f)):
            a, b = f[k], f[(k + 1) % len(f)]
            use[(min(a, b), max(a, b))] = use.get((min(a, b), max(a, b)), 0) + 1
    return sum(1 for c in use.values() if c > 2)


def remesh_paraboloid(tmp: str, target: int):
    src = os.path.join(tmp, "par.obj")
    write_paraboloid(src)
    out_path = os.path.join(tmp, f"out_{target}.obj")
    with Mesh.load_obj(src) as mesh:
        params = RemeshParams(target_quad_count=target, pure_quads=True,
                              quad_method="quad-cover")
        with remesh(mesh, params) as out:
            out.save_obj(out_path)
    return read_obj(out_path)


def main() -> int:
    try:
        with tempfile.TemporaryDirectory() as tmp:
            return _run(tmp)
    except Exception as exc:  # library missing / unloadable -> CTest SKIP
        if "LibraryNotFound" in type(exc).__name__ or "Failed to load" in str(exc):
            print(f"SKIP: {exc}")
            sys.exit(77)
        raise


def _run(tmp: str) -> int:
    # A low target is where the pre-fix path collapsed to a handful of huge faces.
    verts, faces = remesh_paraboloid(tmp, 900)
    quads = [f for f in faces if len(f) == 4]

    # Floor catches the pre-fix ~92-face collapse on both backends; the native solver undershoots
    # the request (~164 quads) where Geogram traces it fully (~1744).
    check("open surface traces densely (not the ~90-face under-trace)",
          len(quads) > 130, f"got {len(quads)} quads")
    # Edge-length CV is the primary discriminator: the pre-fix simplifyGraph path merged
    # cells into long uneven quads (~0.93 here, ~1.7 on the Geogram path), while the fix
    # keeps them uniform. This bound briefly went to 0.7: the isoline graph used to iterate
    # in hash order, which made the traced mesh a property of the standard library
    # (libstdc++ landed on 0.40, libc++ on 0.73), and ordering it settled both toolchains
    # on 0.625. Fixing the smoother's Jacobi two-cycle then brought it to 0.35 -- better
    # than either toolchain managed before -- so the original bound is back.
    cv = edge_cv(verts, faces)
    check("edge-length CV stays uniform (pre-fix simplifyGraph blew it up)",
          cv < 0.6, f"cv={cv:.3f}")
    check("output is all quads", len(quads) == len(faces),
          f"{len(faces) - len(quads)} non-quads")
    check("output is manifold", nonmanifold_edges(faces) == 0,
          f"{nonmanifold_edges(faces)} non-manifold edges")

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed: {FAILURES}")
        return 1
    print("\nall open-surface cleanup checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
