#!/usr/bin/env python3
"""Regressions for where the bindings are allowed to look for the C ABI library.

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_library_lookup.py

The loader used to walk six ancestor levels of the *installed* package for
``build/``-style trees, so a package installed under a shared directory would
dlopen a ``libcyber_capi.so`` planted there by another local user — arbitrary
code in the consumer's process, since ELF constructors run before any name or
version check. The same walk also let a stale build tree silently shadow a
correctly installed library. The rule now is: only a real checkout of this
project counts, and only when no other user can write to it.

Needs no shared library — nothing here is ever dlopened — so it runs everywhere.
"""

import os
import shutil
import sys
import tempfile

# Make the package importable when run directly from a source checkout.
_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

from cyberremesh import _ffi


def _plant(directory, name):
    """Create an empty stand-in for the shared library and return its path."""
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, name)
    with open(path, "wb"):
        pass
    return os.path.abspath(path)


def _lookup_from(package_dir):
    """Run the search as if the package were installed at ``package_dir``.

    ``_candidate_dirs()`` derives everything from the module's own location, so
    pointing ``__file__`` at the fake install is enough — and CTest's
    ``CYBER_CAPI_LIB`` has to go, or it would short-circuit the whole search.
    """
    real_file = _ffi.__file__
    env = os.environ.pop("CYBER_CAPI_LIB", None)
    _ffi.__file__ = os.path.join(package_dir, "_ffi.py")
    try:
        return _ffi.find_library_path(), list(_ffi._candidate_dirs())
    finally:
        _ffi.__file__ = real_file
        if env is not None:
            os.environ["CYBER_CAPI_LIB"] = env


def _make_checkout(root):
    """Lay out the markers that identify a checkout of this project."""
    os.makedirs(os.path.join(root, "capi", "include"), exist_ok=True)
    for marker in ("CMakeLists.txt", os.path.join("capi", "include", "cyber_capi.h")):
        with open(os.path.join(root, marker), "w") as fh:
            fh.write("")


def _check_planted_library_outside_a_checkout_is_ignored(tmp):
    """A venv under a shared directory must not load an ancestor's build tree."""
    shared = os.path.join(tmp, "shared")
    package = os.path.join(shared, "venv", "lib", "python3", "site-packages",
                           "cyberremesh")
    os.makedirs(package)
    planted = _plant(os.path.join(shared, "build", "lib"), _ffi._lib_filenames()[0])

    found, candidates = _lookup_from(package)
    assert found != planted, "loader resolved a planted library: {0}".format(found)
    assert os.path.dirname(planted) not in candidates, candidates
    print("PASS: planted library outside a checkout is ignored")


def _check_developer_build_tree_still_found(tmp):
    """The convenience this guard protects: an in-checkout build still wins."""
    root = os.path.join(tmp, "checkout")
    package = os.path.join(root, "python", "cyberremesh", "cyberremesh")
    os.makedirs(package)
    _make_checkout(root)
    built = _plant(os.path.join(root, "build", "capi"), _ffi._lib_filenames()[0])

    found, _ = _lookup_from(package)
    assert found == built, "in-tree build no longer discovered: {0}".format(found)
    print("PASS: in-checkout build tree is still discovered")


def _check_world_writable_checkout_is_ignored(tmp):
    """Repo markers are forgeable, so ownership/permissions decide as well."""
    if os.name != "posix":
        print("SKIP: permission check is POSIX-only")
        return
    root = os.path.join(tmp, "world-writable")
    package = os.path.join(root, "python", "cyberremesh", "cyberremesh")
    os.makedirs(package)
    _make_checkout(root)
    planted = _plant(os.path.join(root, "build", "capi"), _ffi._lib_filenames()[0])
    os.chmod(root, 0o777)

    found, _ = _lookup_from(package)
    assert found != planted, "loader trusted a world-writable tree: {0}".format(found)
    print("PASS: world-writable checkout is ignored")


def main():
    tmp = tempfile.mkdtemp(prefix="cyberremesh_lookup_")
    try:
        _check_planted_library_outside_a_checkout_is_ignored(tmp)
        _check_developer_build_tree_still_found(tmp)
        _check_world_writable_checkout_is_ignored(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("library lookup tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
