#!/usr/bin/env python3
"""Python package <-> C ABI coverage guard.

The Swift lane has had a parity gate since it drifted; Python never did. What
stood in for one was a hand-written assertion in ``test_abi_contract.py``
listing about six functions, under a comment claiming "engine-bindings requires
anything the C ABI can do to be reachable from Python". By v0.9.0, 71 entry
points were unbound and that assertion was green — including the entire stroke
grammar, on the binding that exists to be the full-surface desktop test harness.

So this is the same check the Swift lane runs, against the same shared module:
every declared ``cyber_*`` entry point is referenced by the package's sources or
listed in PENDING_REGISTRATIONS with a reason.

It needs no built library. It reads the header and the ``.py`` files, so it runs
on any machine, including the lanes where the engine will not load — which is
where a coverage gap is most likely to go unnoticed.

Comments and docstrings are stripped before scanning: a symbol named only in
prose is not bound, and counting it would let the package document its way to
green.
"""

import io
import re
import sys
import tokenize
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import binding_parity  # noqa: E402  (path set above so the shared check imports)

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "capi" / "include" / "cyber_capi.h"
PACKAGE = REPO / "python" / "cyberremesh" / "cyberremesh"

# Entry points deliberately NOT bound in Python, each with the reason.
#
# The rules are the same as the Swift list's: adding a line is cheap, adding one
# without a reason is what review is for, and the gate fails if a line here
# names something the header dropped OR something Python now binds.
PENDING_REGISTRATIONS: dict[str, str] = {
    # --- superseded by a richer variant Python already binds ---------------
    "cyber_mesh_free": "superseded: Python binds cyber_mesh_destroy",
    "cyber_default_params": "superseded: Python binds cyber_remesh_params_default",
    # --- renderer fast paths ----------------------------------------------
    # Borrowed pointers into engine buffers, for a renderer uploading straight
    # to the GPU. Python has the copying accessors, which is the right default
    # for a language where a borrowed pointer outliving its mesh is a segfault
    # rather than a compile error.
    "cyber_mesh_positions_ptr": "renderer fast path",
    "cyber_mesh_normals_ptr": "renderer fast path",
    "cyber_mesh_colors_ptr": "renderer fast path",
    "cyber_mesh_triangle_indices_ptr": "renderer fast path",
    "cyber_mesh_edge_indices_ptr": "renderer fast path",
    "cyber_mesh_tagged_edge_indices_ptr": "renderer fast path",
    "cyber_mesh_copy_render_positions": "renderer fast path",
    "cyber_mesh_copy_colors": "renderer fast path",
    "cyber_mesh_has_colors": "renderer fast path",
    # --- cancellable variants ---------------------------------------------
    # The blocking forms are bound. The cancellable ones need a callback
    # trampoline with the GIL released, which is a behaviour change rather
    # than a binding.
    "cyber_uv_atlas_cancellable": "needs a cancellable trampoline",
    "cyber_uv_unwrap_seams_cancellable": "needs a cancellable trampoline",
    # --- backend selection -------------------------------------------------
    # The desktop harness runs CPU deliberately, so its results are comparable
    # across machines; a backend switch would make the golden tests
    # machine-dependent.
    "cyber_available_backends": "harness is CPU-only by design",
    "cyber_active_backend": "harness is CPU-only by design",
    "cyber_active_backend_name": "harness is CPU-only by design",
    "cyber_set_backend": "harness is CPU-only by design",
    "cyber_set_max_worker_threads": "harness is CPU-only by design",
    "cyber_max_worker_threads": "harness is CPU-only by design",
    # --- retopology follow-ups --------------------------------------------
    # Wanted, not yet written. Listed rather than invisible.
    "cyber_retopo_apply_symmetry": "retopology follow-up",
    "cyber_retopo_resymmetrize": "retopology follow-up",
    "cyber_retopo_snap_symmetry_plane": "retopology follow-up",
}

STRING_PREFIX = re.compile(r"^[rRbBuUfF]{0,2}('''|\"\"\"|'|\")")


def strip_python_noise(text: str) -> str:
    """Remove comments and string literals, keeping line structure.

    Tokenizing rather than regexing, because a `#` inside a string and a
    docstring naming an entry point are exactly the two cases a regex gets
    wrong, and both would make an unbound symbol look bound.
    """
    out = []
    try:
        tokens = tokenize.generate_tokens(io.StringIO(text).readline)
        for kind, value, _start, _end, _line in tokens:
            if kind == tokenize.COMMENT:
                continue
            if kind == tokenize.STRING and STRING_PREFIX.match(value):
                out.append('""')
                continue
            out.append(value)
    except (tokenize.TokenError, IndentationError):
        # A file we cannot tokenize is scanned raw rather than skipped: a false
        # "bound" is worse than a false "unbound", and skipping silently is how
        # a gate stops covering something.
        return text
    return " ".join(out)


def python_files() -> list[Path]:
    return sorted(p for p in PACKAGE.rglob("*.py") if "__pycache__" not in p.parts)


def main() -> int:
    if not HEADER.is_file():
        print(f"FAIL: missing header {HEADER}")
        return 1
    files = python_files()
    if not files:
        print(f"FAIL: no Python sources under {PACKAGE}")
        return 1

    sources = {path: strip_python_noise(path.read_text()) for path in files}
    header_text = HEADER.read_text()

    unbound, stale, redundant = binding_parity.coverage(
        sources, header_text, PENDING_REGISTRATIONS
    )
    lines = binding_parity.report("Python", unbound, stale, redundant, PENDING_REGISTRATIONS)
    for line in lines:
        print(line)
    if lines:
        print(f"\n{len(unbound)} unbound, unregistered entry point(s)")
        print(f"{len(stale)} stale pending registration(s)")
        print(f"{len(redundant)} registration(s) for entry points that ARE bound")
        return 1

    print(f"{len(files)} Python file(s) scanned with comments and strings stripped")
    print(binding_parity.summary("Python", header_text, PENDING_REGISTRATIONS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
