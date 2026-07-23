#!/usr/bin/env python3
"""Hole-fill policy (remeshing-pipeline spec, "Explicit cleanup policies").

The spec requires hole filling to be governed by a user-visible parameter
rather than a hard-coded constant, and that a boundary loop longer than the
configured limit "SHALL be left open".

Only `field-aligned` honours that. The default `quad-cover` and the opt-in
`instant-meshes` extractors decide during extraction, before
`holeFillMaxBoundary` is consulted as a post-pass (src/core/src/pipeline.cpp),
so the parameter cannot influence the result:

    method          limit=0                 limit=64
    quad-cover      hole FILLED (wrong)     hole filled
    field-aligned   hole left open  (ok)    hole filled  (ok)
    instant-meshes  hole left open          hole NOT filled (wrong)

This test locks in the behaviour that is correct and *characterises* the
behaviour that is not, so the violation is visible in the suite rather than
being discovered from a misleading example (examples/06_hole_fill.py printed
"hole left open" for a mesh whose hole it had just filled).

The quad-cover expectation below deliberately asserts the buggy result: when
the extractor learns to respect the limit, this test fails and must be updated.
That is the intent — a tripwire, not an endorsement.

Verified pre-existing: reproduces on a build predating the M3 open-surface work
and on both the Geogram and dependency-free native backends.

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


def write_plane_with_hole(path: str, n: int = 12) -> None:
    """Flat (n+1)^2 triangle grid with the centre quad removed."""
    lines = []
    for j in range(n + 1):
        for i in range(n + 1):
            lines.append(f"v {i / n - 0.5:.6f} 0.0 {j / n - 0.5:.6f}")
    mid = n // 2
    for j in range(n):
        for i in range(n):
            if i == mid and j == mid:
                continue  # the hole
            a = j * (n + 1) + i + 1  # OBJ indices are 1-based
            b, c, d = a + 1, a + n + 1, a + n + 2
            lines.append(f"f {a} {c} {b}")
            lines.append(f"f {b} {c} {d}")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


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

        # quad-cover is the DEFAULT and fills regardless of the limit.
        check("quad-cover fills at limit=64",
              loops_after_remesh(tmp, src, "quad-cover", 64) == 1)
        check("KNOWN GAP: quad-cover ignores limit=0 and still fills",
              loops_after_remesh(tmp, src, "quad-cover", 0) == 1,
              "hole now left open -> the bug is FIXED; update this assertion")

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
