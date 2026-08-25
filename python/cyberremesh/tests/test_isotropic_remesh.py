#!/usr/bin/env python3
"""Isotropic (triangle) remeshing through the bindings.

Covers the engine-bindings spec requirement "Isotropic remeshing binding":

  * parity — every isotropic ABI entry point published in
    ``capi/include/cyber_capi.h`` is bound in ``_ffi.py`` and reachable from
    ``api.py``;
  * ``target_edge_length`` really controls output density;
  * ``adaptivity`` really reaches the engine rather than being dropped in the
    marshalling;
  * a quad mesh is triangulated rather than refused;
  * an unusable target edge length fails loudly instead of being invented.

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_isotropic_remesh.py

Skips with 77 (CTest SKIP) when the C ABI shared library is not loadable.
"""

import math
import os
import re
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh
from cyberremesh import CyberError, IsotropicParams, Mesh

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def _read(path):
    with open(os.path.join(_REPO, path), "r", encoding="utf-8") as handle:
        return handle.read()


def _sphere_obj(radius_z=1.0, rings=12, segments=16):
    """A UV sphere as OBJ text: quad body, triangle fans at the poles.

    ``radius_z`` squashes it into an ellipsoid — a sphere has uniform
    curvature, so the adaptive scale field normalizes to 1 everywhere and
    adaptivity is a no-op on it by design.
    """
    lines = ["v 0 0 {0}".format(radius_z)]
    for i in range(1, rings):
        theta = math.pi * i / rings
        for j in range(segments):
            phi = 2.0 * math.pi * j / segments
            lines.append("v {0} {1} {2}".format(
                math.sin(theta) * math.cos(phi),
                math.sin(theta) * math.sin(phi),
                radius_z * math.cos(theta)))
    lines.append("v 0 0 {0}".format(-radius_z))
    south = 2 + (rings - 1) * segments

    def ring(r, s):
        return 2 + (r - 1) * segments + s % segments

    faces = ["f 1 {0} {1}".format(ring(1, s), ring(1, s + 1)) for s in range(segments)]
    for r in range(1, rings - 1):
        for s in range(segments):
            faces.append("f {0} {1} {2} {3}".format(
                ring(r, s), ring(r + 1, s), ring(r + 1, s + 1), ring(r, s + 1)))
    faces += ["f {0} {1} {2}".format(south, ring(rings - 1, s + 1), ring(rings - 1, s))
              for s in range(segments)]
    return "\n".join(lines + faces) + "\n"


def _write(tmpdir, name, text):
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _positions(mesh):
    flat = mesh._copy_positions()
    return [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]


def check_parity():
    """Every isotropic ABI symbol is bound in ctypes and reachable from api.py."""
    header = _read("capi/include/cyber_capi.h")
    symbols = sorted(set(re.findall(r"\bcyber_\w*isotropic\w*", header)))
    assert symbols == ["cyber_default_isotropic_params", "cyber_mesh_isotropic_remesh"], symbols

    ffi = _read("python/cyberremesh/cyberremesh/_ffi.py")
    api = _read("python/cyberremesh/cyberremesh/api.py")
    for symbol in symbols:
        assert symbol in ffi, "{0}: no ctypes declaration in _ffi.py".format(symbol)
    # The defaults filler is mirrored by IsotropicParams' own field defaults
    # rather than called, so only the remesh has to appear in api.py.
    assert "cyber_mesh_isotropic_remesh" in api, "not reachable from api.py"

    # The ctypes struct must mirror the header field-for-field, in order: a
    # silent layout mismatch reads garbage instead of failing loudly.
    body = re.search(
        r"typedef struct CyberIsotropicParams \{(.*?)\} CyberIsotropicParams;",
        header, re.DOTALL).group(1)
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.DOTALL)
    c_fields = re.findall(r"\b(\w+)\s*;", body)
    py_fields = [name for name, _ in cyberremesh._ffi.CyberIsotropicParams._fields_]
    assert len(c_fields) == len(py_fields), (c_fields, py_fields)
    for c_name, py_name in zip(c_fields, py_fields):
        # camelCase on the C side, snake_case on the Python side.
        expected = re.sub(r"(?<!^)(?=[A-Z])", "_", c_name).lower()
        assert py_name == expected, (c_name, py_name)

    # The dataclass mirrors the same fields, so a caller reads one vocabulary.
    assert list(IsotropicParams.__annotations__) == py_fields, IsotropicParams.__annotations__
    print("PASS parity: {0} isotropic symbols bound, {1} struct fields mirrored".format(
        len(symbols), len(py_fields)))


def check_target_edge_length_controls_density(tmpdir):
    """The whole point of the entry point: a smaller target means more faces."""
    path = _write(tmpdir, "sphere_density.obj", _sphere_obj())

    def remesh_at(target):
        with Mesh.load(path) as mesh:
            faces = mesh.isotropic_remesh(target)
            assert faces == mesh.face_count, (faces, mesh.face_count)
            return faces

    coarse = remesh_at(0.40)
    fine = remesh_at(0.15)
    assert fine > 4 * coarse, (coarse, fine)
    print("PASS density: target 0.40 -> {0} faces, 0.15 -> {1}".format(coarse, fine))


def check_adaptivity_reaches_the_engine(tmpdir):
    """Two runs differing only in ``adaptivity`` cannot agree unless the value
    is being dropped on the way in — the failure mode the C ABI's
    ``sharpEdgeDegrees`` regression already had once."""
    path = _write(tmpdir, "ellipsoid.obj", _sphere_obj(radius_z=0.35, rings=14, segments=22))

    def remesh_at(adaptivity):
        with Mesh.load(path) as mesh:
            mesh.isotropic_remesh(
                IsotropicParams(target_edge_length=0.12, adaptivity=adaptivity))
            return _positions(mesh)

    uniform = remesh_at(0.0)
    adaptive = remesh_at(1.0)
    assert uniform != adaptive, "adaptivity never reached the engine"
    # Same value twice is still deterministic: the knob, not run-to-run noise.
    assert remesh_at(1.0) == adaptive
    print("PASS adaptivity: uniform {0} vs adaptive {1} vertices".format(
        len(uniform), len(adaptive)))


def _face_sizes(path):
    """Corner counts of every face in an OBJ, as a set."""
    with open(path) as fh:
        return set(len(line.split()) - 1 for line in fh if line.startswith("f "))


def check_quads_are_triangulated(tmpdir):
    """The documented answer to non-triangulated input, and the obvious call:
    densify the quads a remesh just produced."""
    path = _write(tmpdir, "sphere_quads.obj", _sphere_obj(rings=10, segments=14))
    assert _face_sizes(path) == {3, 4}, "fixture should carry both quads and pole triangles"

    out = os.path.join(tmpdir, "sphere_quads_out.obj")
    with Mesh.load(path) as mesh:
        mesh.isotropic_remesh(0.25)
        mesh.save(out)
    assert _face_sizes(out) == {3}, _face_sizes(out)
    print("PASS triangulation: quad sphere came back as pure triangles")


def check_bad_target_fails_loudly(tmpdir):
    """A world-space length has no scale-free default, so a non-positive one is
    refused rather than silently invented."""
    path = _write(tmpdir, "sphere_reject.obj", _sphere_obj(rings=6, segments=8))
    with Mesh.load(path) as mesh:
        before = _positions(mesh)
        for bad in (0.0, -1.0, float("nan")):
            try:
                mesh.isotropic_remesh(bad)
            except CyberError as exc:
                assert "targetEdgeLength" in str(exc), str(exc)
            else:
                raise AssertionError("expected CyberError for target {0!r}".format(bad))
        try:
            mesh.isotropic_remesh(IsotropicParams(0.3, iterations=0))
        except CyberError as exc:
            assert "iterations" in str(exc), str(exc)
        else:
            raise AssertionError("expected CyberError for iterations=0")
        # Every rejection is decided before the mesh is touched.
        assert _positions(mesh) == before
    print("PASS rejection: unusable target and iteration count refused, mesh untouched")


def check_empty_mesh_fails_loudly():
    with Mesh() as mesh:
        try:
            mesh.isotropic_remesh(0.1)
        except CyberError as exc:
            print("PASS: empty mesh rejected ({0})".format(exc))
        else:
            raise AssertionError("expected CyberError remeshing a mesh with no faces")


def main():
    assert hasattr(Mesh, "isotropic_remesh"), "Mesh.isotropic_remesh is missing from the binding"
    check_parity()

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 77

    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_isotropic_")
    check_target_edge_length_controls_density(tmpdir)
    check_adaptivity_reaches_the_engine(tmpdir)
    check_quads_are_triangulated(tmpdir)
    check_bad_target_fails_loudly(tmpdir)
    check_empty_mesh_fails_loudly()
    return 0


if __name__ == "__main__":
    sys.exit(main())
