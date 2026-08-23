#!/usr/bin/env python3
"""Format-agnostic load/save through the bindings.

Covers engine-bindings "Format-agnostic mesh load and save binding" and the
mesh-io FBX rules: FBX loads like any other format, and refuses to be written.

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_io_formats.py

Skips with 77 (CTest SKIP) when the C ABI shared library is not loadable.
"""

import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh
from cyberremesh import CyberError, Mesh

# tests/data lives next to the C++ suite; the FBX fixtures are Blender exports
# (see tests/data/generate_fbx_fixtures.py) because nothing here can write FBX.
_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_DATA = os.path.join(_REPO, "tests", "data")

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

# Writable formats, and whether they keep faces as authored. STL and glTF are
# triangle-only containers, so a 6-quad cube legitimately comes back as 12
# triangles; OBJ and PLY must preserve the quads.
_WRITABLE = [(".obj", 6), (".ply", 6), (".stl", 12), (".gltf", 12), (".glb", 12)]


def _test_round_trip_every_writable_format(tmpdir):
    src = os.path.join(tmpdir, "cube.obj")
    with open(src, "w") as fh:
        fh.write(_CUBE_OBJ)

    for suffix, expected_faces in _WRITABLE:
        out = os.path.join(tmpdir, "cube" + suffix)
        with Mesh.load(src) as mesh:
            mesh.save(out)
        assert os.path.isfile(out), out

        with Mesh.load(out) as reloaded:
            assert reloaded.vertex_count > 0, suffix
            assert reloaded.face_count == expected_faces, (
                suffix, reloaded.face_count, expected_faces)
        print("PASS: {0} round-trips to {1} faces".format(suffix, expected_faces))


def _test_fbx_loads(tmpdir):
    fixture = os.path.join(_DATA, "cube_quads.fbx")
    if not os.path.isfile(fixture):
        raise AssertionError("missing FBX fixture: {0}".format(fixture))

    with Mesh.load(fixture) as mesh:
        assert mesh.vertex_count == 8, mesh.vertex_count
        assert mesh.face_count == 6, mesh.face_count
        # Once loaded, an FBX behaves like anything else: write it out as OBJ.
        out = os.path.join(tmpdir, "from_fbx.obj")
        mesh.save(out)
    assert os.path.isfile(out)
    print("PASS: FBX loaded (8 verts, 6 quads) and re-saved as OBJ")


def _test_fbx_save_is_refused(tmpdir):
    fixture = os.path.join(_DATA, "cube_quads.fbx")
    out = os.path.join(tmpdir, "nope.fbx")
    with Mesh.load(fixture) as mesh:
        try:
            mesh.save(out)
        except CyberError as exc:
            message = str(exc)
            # The error has to be actionable: the same extension imports fine,
            # so a bare "unsupported format" would read like a bug.
            assert "import-only" in message, message
            assert ".obj" in message, message
            print("PASS: FBX save refused ({0})".format(exc))
        else:
            raise AssertionError("expected CyberError writing .fbx")
    assert not os.path.exists(out), "a refused export must not leave a file behind"


def _test_unknown_extension_is_refused(tmpdir):
    # The file has to exist, or the missing-file check fires first and this
    # never reaches the format dispatch it is meant to cover.
    path = os.path.join(tmpdir, "model.wat")
    with open(path, "w") as fh:
        fh.write(_CUBE_OBJ)

    try:
        Mesh.load(path)
    except CyberError as exc:
        message = str(exc)
        assert "unsupported" in message.lower(), message
        assert path in message, message
        print("PASS: unknown extension refused ({0})".format(exc))
    else:
        raise AssertionError("expected CyberError loading an unknown extension")


def _test_obj_aliases_still_work(tmpdir):
    """The `_obj` spellings predate multi-format support and must keep working
    for callers written against them."""
    src = os.path.join(tmpdir, "alias.obj")
    with open(src, "w") as fh:
        fh.write(_CUBE_OBJ)

    out = os.path.join(tmpdir, "alias_out.ply")
    with Mesh.load_obj(src) as mesh:
        assert mesh.face_count == 6
        # Always dispatched on the extension, despite the name.
        mesh.save_obj(out)
    with Mesh.load_obj(out) as reloaded:
        assert reloaded.face_count == 6, reloaded.face_count
    print("PASS: load_obj/save_obj aliases dispatch on the extension")


def main():
    for name in ("load", "save", "load_obj", "save_obj"):
        assert hasattr(Mesh, name), "Mesh.{0} is missing from the binding".format(name)

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 77

    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_io_")
    _test_round_trip_every_writable_format(tmpdir)
    _test_fbx_loads(tmpdir)
    _test_fbx_save_is_refused(tmpdir)
    _test_unknown_extension_is_refused(tmpdir)
    _test_obj_aliases_still_work(tmpdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
