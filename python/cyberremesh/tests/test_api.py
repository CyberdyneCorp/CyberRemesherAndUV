#!/usr/bin/env python3
"""Integration test for the cyberremesh bindings.

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_api.py

Behaviour:
  * Imports the package WITHOUT dlopening anything — this must always succeed,
    even on a machine that never built the engine.
  * If the C ABI shared library cannot be found, prints ``SKIP`` and exits 77 (CTest SKIP)
    (this is an integration test, gated on the built `capi` module).
  * Otherwise loads a temporary cube OBJ, remeshes it and asserts that the
    result contains quads.
"""

import os
import re
import sys
import tempfile

# Make the package importable when run directly from a source checkout.
_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

_REPO = os.path.dirname(  # <repo>/python/cyberremesh/tests -> <repo>
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)

import cyberremesh
from cyberremesh import CyberError, Mesh, RemeshLimits, RemeshParams, remesh

# A unit cube as an OBJ (8 verts, 6 quad faces).
_CUBE_OBJ = """\
v -0.5 -0.5 -0.5
v  0.5 -0.5 -0.5
v  0.5  0.5 -0.5
v -0.5  0.5 -0.5
v -0.5 -0.5  0.5
v  0.5 -0.5  0.5
v  0.5  0.5  0.5
v -0.5  0.5  0.5
f 1 2 3 4
f 5 8 7 6
f 1 5 6 2
f 2 6 7 3
f 3 7 8 4
f 4 8 5 1
"""


def _check_import_contract():
    # Import-time must not have loaded the shared library.
    assert hasattr(cyberremesh, "Mesh")
    assert hasattr(cyberremesh, "RemeshParams")
    assert hasattr(cyberremesh, "remesh")
    assert hasattr(cyberremesh, "CyberError")
    assert issubclass(CyberError, RuntimeError)


def _check_library_discovery():
    """Regression: ``_lib_filenames()`` only listed ``libcyber_capi_shared.*``,
    a name the build never emits (capi/CMakeLists.txt sets ``OUTPUT_NAME
    cyber_capi``), and the probed directories never included
    ``build/<preset>/capi`` where the CMake presets put it — so every documented
    search step except ``CYBER_CAPI_LIB`` was dead, and CTest injecting that env
    var hid it. Needs no shared library, so it runs everywhere."""
    from cyberremesh import _ffi

    with open(os.path.join(_REPO, "capi", "CMakeLists.txt")) as fh:
        output_name = re.search(r"OUTPUT_NAME\s+(\S+)", fh.read()).group(1)
    names = _ffi._lib_filenames()
    assert any(output_name in name for name in names), (output_name, names)

    # Drop the env var CTest sets and prove auto-discovery finds the same
    # in-tree build on its own.
    env = os.environ.pop("CYBER_CAPI_LIB", None)
    try:
        found = _ffi.find_library_path()
    finally:
        if env is not None:
            os.environ["CYBER_CAPI_LIB"] = env
    if env and os.path.isfile(env):
        assert found is not None, "auto-discovery missed the built library"
    print("PASS: library discovery covers {0} ({1})".format(
        output_name, "resolved" if found else "no build tree present"))


def _run_remesh():
    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_test_")
    obj_path = os.path.join(tmpdir, "cube.obj")
    out_path = os.path.join(tmpdir, "out.obj")
    with open(obj_path, "w") as fh:
        fh.write(_CUBE_OBJ)

    progress_seen = []

    def on_progress(fraction, stage):
        progress_seen.append((fraction, stage))

    with Mesh.load_obj(obj_path) as mesh:
        assert mesh.vertex_count == 8, mesh.vertex_count
        result = remesh(
            mesh,
            RemeshParams(target_quad_count=200),
            progress=on_progress,
            cancel=lambda: False,
        )
        with result:
            assert result.stats is not None
            assert result.stats.quads > 0, result.stats
            result.save_obj(out_path)

    assert os.path.isfile(out_path)
    print("PASS: remesh produced {0} quads".format(
        # re-report from a fresh load to prove the file round-trips
        _quads_in(out_path)
    ))


def _run_quad_method():
    # Both quadrangulators must be selectable and produce quads; an unknown
    # method name must raise before touching the engine.
    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_qm_")
    obj_path = os.path.join(tmpdir, "cube.obj")
    with open(obj_path, "w") as fh:
        fh.write(_CUBE_OBJ)

    # zremesher is included: the engine-bindings spec requires every quad method
    # reachable from the CLI to be reachable from Python, and a method that is
    # only in the enum is not actually bound.
    for method in ("field-aligned", "instant-meshes", "zremesher"):
        with Mesh.load_obj(obj_path) as mesh:
            result = remesh(mesh, RemeshParams(target_quad_count=200, quad_method=method))
            with result:
                assert result.stats.quads > 0, (method, result.stats)
                if method == "zremesher":
                    report = result.zremesher_report
                    assert report is not None
                    injectability = report.injectability
                    assert injectability is not None, "additive injectability ABI was not mapped"
                    attributed = (injectability.injectable_arcs + injectability.excluded_arcs +
                                  injectability.empty_rows + injectability.lattice_free_rows +
                                  injectability.fractional_coefficient_rows)
                    assert attributed == injectability.arcs, (
                        injectability, attributed, injectability.arcs)
        print("PASS: quad_method={0} produced {1} quads".format(method, result.stats.quads))

    try:
        RemeshParams(quad_method="nope")._to_c()
    except ValueError:
        print("PASS: unknown quad_method rejected")
    else:
        raise AssertionError("expected ValueError for unknown quad_method")


def _run_guide_point_arity():
    # Regression: the guide packing sized its ctypes buffer from the flattened
    # points but reported point_count = len(guide.points), so a 2-component
    # point made the C ABI read 3 * point_count floats past the buffer and
    # place the guide at a world position built from heap bytes.
    from cyberremesh import FlowGuide

    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_guide_")
    obj_path = os.path.join(tmpdir, "cube.obj")
    with open(obj_path, "w") as fh:
        fh.write(_CUBE_OBJ)

    with Mesh.load_obj(obj_path) as mesh:
        bad = FlowGuide(points=[(0.0, 0.0), (1.0, 1.0)], radius=0.1)
        try:
            remesh(mesh, RemeshParams(target_quad_count=200), guides=[bad])
        except ValueError as exc:
            assert "3 components per point" in str(exc), exc
        else:
            raise AssertionError("expected ValueError for a 2-component guide point")

        good = FlowGuide(points=[(-0.5, 0.0, 0.0), (0.5, 0.0, 0.0)], radius=0.5)
        result = remesh(mesh, RemeshParams(target_quad_count=200), guides=[good])
        with result:
            assert result.stats.quads > 0, result.stats
    print("PASS: guide point arity checked in the binding")


def _quads_in(path):
    quads = 0
    with open(path) as fh:
        for line in fh:
            if line.startswith("f ") and len(line.split()) == 5:
                quads += 1
    return quads


def _run_bulk_indexed_exchange():
    """CSR topology and all three attribute domains cross one copied ABI call."""
    with Mesh.from_indexed(
        [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], [0, 4], [0, 1, 2, 3],
        {("vertex", "weight"): [1.0, 2.0, 3.0, 4.0],
         ("face", "material"): [7],
         ("corner", "uv"): [(0, 0), (1, 0), (1, 1), (0, 1)]},
    ) as mesh:
        assert mesh.authored_polygons() == ([0, 4], [0, 1, 2, 3])
        attributes = mesh.authored_attributes()
        assert attributes[("face", "material")] == [7]
        assert attributes[("corner", "uv")] == [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    print("PASS: bulk indexed polygon and attribute exchange")


def _run_semantic_boundary_report():
    """A face-domain group id reaches the returned ZRemesher evidence."""
    positions = [0, 0, 0, 1, 0, 0, 0.5, 0.866, 0, 0.5, 0.289, 0.816]
    offsets = [0, 3, 6, 9, 12]
    indices = [0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3]
    with Mesh.from_indexed(
        positions, offsets, indices, {("face", "group_id"): [1, 0, 0, 0]}
    ) as mesh:
        result = remesh(
            mesh, RemeshParams(target_quad_count=100, quad_method="zremesher"),
            limits=RemeshLimits(),
        )
        with result:
            report = result.zremesher_report
            assert report is not None and report.semantic_boundaries is not None
            semantic = report.semantic_boundaries
            assert len(semantic.boundaries) == 1, semantic
            assert semantic.boundaries[0].id.startswith("group_id:")
            assert semantic.boundaries[0].state != "rejected"
    print("PASS: semantic boundary evidence reaches Python")


def main():
    _check_import_contract()
    _check_library_discovery()

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 77  # CTest SKIP_RETURN_CODE — reported as Skipped, never a vacuous pass

    print("cyberremesh engine version: {0}".format(cyberremesh.version()))
    _run_remesh()
    _run_quad_method()
    _run_guide_point_arity()
    _run_bulk_indexed_exchange()
    _run_semantic_boundary_report()
    return 0


if __name__ == "__main__":
    sys.exit(main())
