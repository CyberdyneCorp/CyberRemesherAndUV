#!/usr/bin/env python3
"""Smoke test for the Python examples (engine-bindings spec: the offline
examples are registered with CTest).

The examples are the project's showcase and the most-run Python code in the
tree, but nothing executed them in the suite — a binding rename or a helper
signature change could break the whole gallery unnoticed. This runs the ones
that need neither the network nor a downloaded model, end to end, and asserts
each wrote its PNG.

    python python/cyberremesh/tests/test_examples.py

Skips with 77 (CTest SKIP) when the engine or matplotlib/NumPy are missing.
The heavier examples (benchmarks, model corpora, reference comparisons) are
deliberately out of scope: they download models and shell out to QuadriFlow.
"""

import os
import subprocess
import sys
import tempfile
import time

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_EXAMPLES = os.path.join(_REPO, "examples")

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh

# (script, the PNG it must produce). Offline and procedural only.
_EXAMPLE_SCRIPTS = [
    ("01_quad_remesh.py", "01_quad_remesh.png"),
    ("04_sharp_edges.py", "04_sharp_edges.png"),
    ("05_pure_quads.py", "05_pure_quads.png"),
    ("06_hole_fill.py", "06_hole_fill.png"),
    ("14_uv_atlas.py", "14_uv_atlas.png"),
    ("16_subdivide.py", "16_subdivide.png"),
    ("21_topology_layout.py", "21_topology_layout.png"),
]

# Generous: these run the real pipeline, and CI runners are slow.
_TIMEOUT_SECONDS = 900


def _have_plotting():
    try:
        import matplotlib  # noqa: F401
        import numpy  # noqa: F401
    except ImportError:
        return False
    return True


def _run_example(script, png, output_dir):
    path = os.path.join(_EXAMPLES, script)
    assert os.path.isfile(path), "missing example: {0}".format(path)

    out_png = os.path.join(output_dir, png)

    env = dict(os.environ)
    # The examples import `common` as a top-level module and the package from
    # the source tree; run them the way examples/run.sh does.
    env["PYTHONPATH"] = os.pathsep.join(
        [_EXAMPLES, _PKG_PARENT, env.get("PYTHONPATH", "")]
    )
    env.setdefault("MPLBACKEND", "Agg")
    # Render into scratch: re-rendering examples/output/ would rewrite the
    # committed gallery PNGs (matplotlib versions differ) and dirty the repo.
    env["CYBER_EXAMPLES_OUTPUT"] = output_dir

    started = time.time()
    proc = subprocess.run(
        [sys.executable, path],
        cwd=_REPO,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=_TIMEOUT_SECONDS,
    )
    elapsed = time.time() - started
    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise AssertionError(
            "example {0} failed (exit {1}):\n{2}".format(script, proc.returncode, output))

    assert os.path.exists(out_png), "{0} produced no {1}:\n{2}".format(script, png, output)
    assert os.path.getsize(out_png) > 0, "{0} wrote an empty {1}".format(script, png)
    print("PASS: {0} -> {1} ({2:.1f}s)".format(script, png, elapsed))


def main():
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 77
    if not _have_plotting():
        print("SKIP: examples need matplotlib and numpy")
        return 77

    output_dir = tempfile.mkdtemp(prefix="cyberremesh_examples_")
    for script, png in _EXAMPLE_SCRIPTS:
        _run_example(script, png, output_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
