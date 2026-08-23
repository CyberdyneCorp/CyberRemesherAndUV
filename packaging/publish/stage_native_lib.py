#!/usr/bin/env python3
"""Stage the built C ABI shared library into the Python package directory.

The `cyberremesh` wheel is a ctypes wrapper: it compiles nothing, and
`cyberremesh._ffi.find_library_path` looks for the library inside the installed
package directory (candidate #2). Nothing used to put it there, so the publish
lane could only ever emit a `py3-none-any` wheel holding three `.py` files —
importable, but `LibraryNotFound` on the first engine call. This script is the
missing step, run from `CIBW_BEFORE_ALL` between the CMake build and the wheel
build.

    stage_native_lib.py <cmake-build-dir> <package-dir>

The library is copied under its plain, unversioned name (`libcyber_capi.so`,
`libcyber_capi.dylib`, `cyber_capi.dll`) because that is the first name
`_ffi._lib_filenames()` tries and the only one a `*.so` package-data glob keeps.
Exits nonzero when no library is found, so a broken build fails the wheel lane
rather than shipping a wheel with no engine in it.
"""

from __future__ import annotations

import os
import re
import shutil
import sys
from pathlib import Path

# Matches the artifact under any of CMake's naming schemes: `libcyber_capi.so`,
# the versioned soname `libcyber_capi.so.0.5.0`, macOS `libcyber_capi.0.dylib`,
# and Windows `cyber_capi.dll` (MinGW prefixes it with `lib`).
_PATTERNS = (
    (re.compile(r"^libcyber_capi\.so(\.\d+)*$"), "libcyber_capi.so"),
    (re.compile(r"^libcyber_capi(\.\d+)*\.dylib$"), "libcyber_capi.dylib"),
    (re.compile(r"^(lib)?cyber_capi\.dll$"), "cyber_capi.dll"),
)


def find_native_lib(build_dir: Path) -> "tuple[Path, str] | None":
    """Return (source path, canonical destination name) for the built library.

    Prefers the shortest path so a top-level `capi/libcyber_capi.so` wins over a
    copy that landed deeper in the tree, and a real file over a symlink so the
    wheel carries content rather than a dangling link.
    """
    matches: "list[tuple[int, int, Path, str]]" = []
    for root, _dirs, files in os.walk(build_dir):
        for name in files:
            for pattern, canonical in _PATTERNS:
                if pattern.match(name):
                    path = Path(root) / name
                    matches.append((len(path.parts), int(path.is_symlink()), path, canonical))
    if not matches:
        return None
    matches.sort(key=lambda m: (m[1], m[0], str(m[2])))
    _, _, path, canonical = matches[0]
    return path, canonical


def main(argv: "list[str]") -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    build_dir, package_dir = Path(argv[1]), Path(argv[2])
    if not build_dir.is_dir():
        print(f"stage_native_lib: build dir {build_dir} does not exist", file=sys.stderr)
        return 1
    if not package_dir.is_dir():
        print(f"stage_native_lib: package dir {package_dir} does not exist", file=sys.stderr)
        return 1

    found = find_native_lib(build_dir)
    if found is None:
        print(f"stage_native_lib: no cyber_capi shared library under {build_dir} — "
              f"the wheel would ship no engine", file=sys.stderr)
        return 1

    source, canonical = found
    dest = package_dir / canonical
    # Resolve symlinks: the wheel must carry the real object.
    shutil.copyfile(os.path.realpath(source), dest)
    shutil.copymode(os.path.realpath(source), dest)
    print(f"stage_native_lib: {source} -> {dest} ({dest.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
