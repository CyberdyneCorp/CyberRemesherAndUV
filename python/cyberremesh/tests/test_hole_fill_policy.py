#!/usr/bin/env python3
"""Hole-fill policy (remeshing-pipeline spec, "Explicit cleanup policies").

The spec requires hole filling to be governed by a user-visible parameter
rather than a hard-coded constant, and that a boundary loop longer than the
configured limit "SHALL be left open".

`holeFillMaxBoundary` is applied as a post-pass over the quadrangulator's
output (src/core/src/pipeline.cpp), so it can only fill loops that SURVIVE
extraction. The default `quad-cover` closes holes *during* extraction, so it
used to ignore the parameter entirely — it had its own hard-coded limit of 65,
the same magic constant the spec criticises AutoRemesher for. The run's policy
is now threaded into the extractor (`extractIsolineQuads` ->
`IsolineExtractor::setHoleFillMaxBoundary`), so:

    method          limit=0             limit=64
    quad-cover      hole left open ok   hole filled ok
    field-aligned   hole left open ok   hole filled ok
    instant-meshes  hole left open      hole NOT filled (still ignores it)

This test is the regression guard. It covers both directions of the parameter
and the over-limit case — a hole LONGER than the limit must stay open even
when filling is enabled, which is the spec's actual wording and what a plain
on/off flag would miss.

`instant-meshes` is a retired opt-in extractor and still ignores the parameter;
it is deliberately not asserted here.

Runnable as a plain script; exits 77 (CTest SKIP) if the library is absent.
"""

import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh  # noqa: E402
from cyberremesh import Mesh, RemeshParams, remesh  # noqa: E402

FAILURES: "list" = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def write_plane_with_hole_of(path: str, n: int = 12, cells: int = 1) -> None:
    """Flat (n+1)^2 triangle grid with a centred `cells` x `cells` hole."""
    lines = []
    for j in range(n + 1):
        for i in range(n + 1):
            lines.append(f"v {i / n - 0.5:.6f} 0.0 {j / n - 0.5:.6f}")
    lo = (n - cells) // 2
    hi = lo + cells
    for j in range(n):
        for i in range(n):
            if lo <= i < hi and lo <= j < hi:
                continue  # the hole
            a = j * (n + 1) + i + 1  # OBJ indices are 1-based
            b, c, d = a + 1, a + n + 1, a + n + 2
            lines.append(f"f {a} {c} {b}")
            lines.append(f"f {b} {c} {d}")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def write_plane_with_hole(path: str, n: int = 12) -> None:
    write_plane_with_hole_of(path, n, cells=1)


def read_faces(path: str) -> "list":
    faces = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("f "):
                continue
            idx = [int(tok.split("/")[0]) for tok in line.split()[1:]]
            faces.append([i - 1 if i > 0 else i for i in idx])
    return faces


def boundary_loop_count(faces: "list") -> int:
    """Connected components of the edges used by exactly one face."""
    counts: "dict" = {}
    for face in faces:
        k = len(face)
        for idx in range(k):
            a, b = face[idx], face[(idx + 1) % k]
            edge = (min(a, b), max(a, b))
            counts[edge] = counts.get(edge, 0) + 1
    adjacency: "dict" = {}
    for (a, b), n in counts.items():
        if n != 1:
            continue
        adjacency.setdefault(a, []).append(b)
        adjacency.setdefault(b, []).append(a)
    seen, loops = set(), 0
    for start in adjacency:
        if start in seen:
            continue
        loops += 1
        stack = [start]
        seen.add(start)
        while stack:
            u = stack.pop()
            for v in adjacency[u]:
                if v not in seen:
                    seen.add(v)
                    stack.append(v)
    return loops


def loops_after_remesh(tmpdir: str, src: str, method: str, limit: int) -> int:
    out_path = os.path.join(tmpdir, f"out_{method}_{limit}.obj")
    with Mesh.load_obj(src) as mesh:
        params = RemeshParams(target_quad_count=800, quad_method=method,
                              hole_fill_max_boundary=limit)
        with remesh(mesh, params) as out:
            out.save_obj(out_path)
    return boundary_loop_count(read_faces(out_path))


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "holed_plane.obj")
        write_plane_with_hole(src)

        check("input has an outer border and a hole",
              boundary_loop_count(read_faces(src)) == 2)

        # field-aligned honours the parameter in both directions. This is the
        # contract the spec describes, and why the others are known-wrong.
        check("field-aligned leaves the hole open at limit=0",
              loops_after_remesh(tmp, src, "field-aligned", 0) == 2)
        check("field-aligned fills the hole at limit=64",
              loops_after_remesh(tmp, src, "field-aligned", 64) == 1)

        # quad-cover is the DEFAULT. It closes holes during extraction, so the
        # limit is threaded into the extractor itself rather than applied as a
        # post-pass; this is the regression guard for that.
        check("quad-cover leaves the hole open at limit=0",
              loops_after_remesh(tmp, src, "quad-cover", 0) == 2)
        check("quad-cover fills the hole at limit=64",
              loops_after_remesh(tmp, src, "quad-cover", 64) == 1)

        # A hole longer than the limit must be left open even when filling is on
        # — the spec's actual wording, and what a single on/off flag would miss.
        big = os.path.join(tmp, "big_hole.obj")
        write_plane_with_hole_of(big, n=24, cells=8)
        check("input with the large hole has two loops",
              boundary_loop_count(read_faces(big)) == 2)
        check("quad-cover leaves an over-limit hole open (limit=8)",
              loops_after_remesh(tmp, big, "quad-cover", 8) == 2)

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("\nhole-fill policy behaviour is as recorded")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except cyberremesh.CyberError as exc:
        print(f"FAIL: engine error {exc}")
        sys.exit(1)
    except Exception as exc:  # library missing -> CTest SKIP
        if "LibraryNotFound" in type(exc).__name__ or "Failed to load" in str(exc):
            print(f"SKIP: {exc}")
            sys.exit(77)
        raise
