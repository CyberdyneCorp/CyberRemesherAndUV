#!/usr/bin/env python3
"""The C ABI contract, checked against the SHIPPED shared library.

openspec/specs/engine-bindings/spec.md has required a runtime-queryable ABI
version with additive-only minors since the bootstrap change. Until
CYBER_ABI_VERSION_* existed there was nothing to query, so the requirement had
no test and the first external embedder pinned by commit and hand-rolled its own
expected-ABI constant instead.

This is the ctypes lane deliberately: it is the only one that goes through the
real dlopen path a host uses, so it catches a header and a built library
disagreeing — the exact failure a soname is supposed to prevent and could not,
while the soname tracked the project's major version and was 0 for every
release.

    python python/cyberremesh/tests/test_abi_contract.py

Skips with 77 (CTest SKIP) when the shared library is not loadable.
"""

import ctypes
import os
import re
import sys

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh  # noqa: E402
from cyberremesh import _ffi  # noqa: E402

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_HEADER = os.path.join(_REPO, "capi", "include", "cyber_capi.h")


def _header_abi():
    """The ABI version as DECLARED in the header, parsed the way a vendoring
    consumer would — bindgen turns these same two macros into constants."""
    text = open(_HEADER, encoding="utf-8").read()
    out = {}
    for part in ("MAJOR", "MINOR"):
        m = re.search(r"^#define\s+CYBER_ABI_VERSION_%s\s+(\d+)" % part, text, re.M)
        assert m, "CYBER_ABI_VERSION_%s missing from the header" % part
        out[part] = int(m.group(1))
    return out["MAJOR"], out["MINOR"]


def gate_header_and_built_library_agree():
    header = _header_abi()
    loaded = cyberremesh.abi_version()
    assert loaded == header, (
        "the built library reports ABI %s but the header declares %s — a stale "
        "build, or CMake stopped reading the header" % (loaded, header)
    )
    print("PASS: header ABI %d.%d matches the loaded library" % header)


def gate_binding_constant_matches_the_header():
    # The binding carries its own copy so check_abi() has a default, which is
    # exactly the drift the Rust consumer's guard exists to catch. Pin it.
    assert (cyberremesh.ABI_VERSION_MAJOR, cyberremesh.ABI_VERSION_MINOR) == _header_abi()
    print("PASS: the Python binding's ABI constant matches the header")


def gate_the_compatibility_rule_is_the_engine_s():
    # A client compiled against this ABI, and any earlier minor, is served.
    cyberremesh.check_abi()
    major, minor = _header_abi()
    for older in range(0, minor + 1):
        cyberremesh.check_abi(major, older)

    # A later minor and a different major are refused, through the SAME engine
    # rule a C or Rust host gets — not a Python reimplementation of it.
    for bad in ((major, minor + 1), (major + 1, 0), (major - 1, 0)):
        try:
            cyberremesh.check_abi(*bad)
        except cyberremesh.CyberError as exc:
            message = str(exc)
            assert "%d.%d" % (major, minor) in message, message
        else:
            raise AssertionError("check_abi accepted an incompatible ABI %s" % (bad,))
    print("PASS: the compatibility rule refuses a newer minor and either major")


def gate_the_abi_is_not_the_engine_version():
    # The whole point of two numbers. If a future change derives one from the
    # other — a rejected design, because the engine version moves for quality
    # work that leaves the surface untouched — this is what fails.
    abi_major, _ = cyberremesh.abi_version()
    engine_major = int(cyberremesh.version().split(".")[0])
    assert abi_major != engine_major, (
        "ABI major %d equals the engine major %d; the two identities have "
        "collapsed into one" % (abi_major, engine_major)
    )
    print("PASS: the ABI version is independent of the engine version")


def gate_the_soname_tracks_the_abi_major():
    # The build's SOVERSION and the loader's expectation are two declarations of
    # one number, in different languages, and nothing else compares them.
    from cyberremesh import _ffi

    abi_major, _ = _header_abi()
    assert _ffi._SOVERSION == abi_major, (
        "_ffi._SOVERSION is %d but the ABI major is %d — the loader will look "
        "for the wrong versioned file name" % (_ffi._SOVERSION, abi_major)
    )
    cmake = open(os.path.join(_REPO, "capi", "CMakeLists.txt"), encoding="utf-8").read()
    assert "SOVERSION ${CYBER_ABI_VERSION_MAJOR}" in cmake, (
        "capi/CMakeLists.txt no longer derives SOVERSION from the ABI major"
    )
    print("PASS: SOVERSION, the loader and the header agree on ABI major %d" % abi_major)


def gate_the_minor_bump_serves_the_previous_minor():
    """This change bumped the ABI minor 1.0 -> 1.1 by ADDING two entry points.

    That is the additive-only rule exercised on itself rather than asserted: a
    host compiled against 1.0 -- before cyber_mesh_topology_generation and
    cyber_set_max_import_vertices existed -- must still be served, because
    nothing it knew about was taken away.
    """
    major, minor = _header_abi()
    assert (major, minor) == (1, 11), (major, minor)
    for older in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9):
        cyberremesh.check_abi(1, older)  # every earlier minor, still served
    print("PASS: ABI 1.11 still serves clients compiled against every earlier 1.x minor")

    limits = _ffi.CyberRemeshLimits()
    _ffi.get_lib().cyber_default_remesh_limits(ctypes.byref(limits))
    assert all(getattr(limits, field) == 0 for field, _ in limits._fields_)
    execution = _ffi.CyberRemeshExecutionLimits()
    _ffi.get_lib().cyber_default_remesh_execution_limits(ctypes.byref(execution))
    assert execution.max_direct_factor_bytes == 0
    assert execution.max_candidate_bytes == 0
    print("PASS: the per-call remesh limits ABI is reachable from Python")


def gate_the_new_entry_points_are_reachable():
    # Parity: engine-bindings requires anything the C ABI can do to be reachable
    # from Python, so an addition that skips the binding is only half-added.
    assert cyberremesh.max_import_vertices() == 0
    cyberremesh.set_max_import_vertices(1_000_000)
    assert cyberremesh.max_import_vertices() == 1_000_000
    cyberremesh.set_max_import_vertices(0)
    assert cyberremesh.max_import_input_bytes() == 0
    cyberremesh.set_max_import_input_bytes(1_000_000)
    assert cyberremesh.max_import_input_bytes() == 1_000_000
    cyberremesh.set_max_import_input_bytes(0)
    assert cyberremesh.max_import_faces() == 0
    cyberremesh.set_max_import_faces(1_000_000)
    assert cyberremesh.max_import_faces() == 1_000_000
    cyberremesh.set_max_import_faces(0)
    assert cyberremesh.max_bake_pixels() == 0
    cyberremesh.set_max_bake_pixels(1_000_000)
    assert cyberremesh.max_bake_pixels() == 1_000_000
    cyberremesh.set_max_bake_pixels(0)
    assert cyberremesh.seamless_solver() in ("native", "native+geogram"), \
        cyberremesh.seamless_solver()
    assert hasattr(cyberremesh, "CountPolicy")
    assert hasattr(cyberremesh, "TargetCountReport")
    print("PASS: the 1.1/1.2/1.3 additions are reachable from Python")


def main():
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77
    gate_header_and_built_library_agree()
    gate_binding_constant_matches_the_header()
    gate_the_compatibility_rule_is_the_engine_s()
    gate_the_abi_is_not_the_engine_version()
    gate_the_soname_tracks_the_abi_major()
    gate_the_minor_bump_serves_the_previous_minor()
    gate_the_new_entry_points_are_reachable()
    print("all ABI contract gates passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
