#!/usr/bin/env python3
"""License audit gate (build-and-packaging spec, "Permissive-license dependency policy").

Checks that:
  1. every dependency in thirdparty/manifest.json declares an allowed license;
  2. every directory under thirdparty/ is listed in the manifest (no stray vendored code);
  3. every listed dependency directory exists and contains a license file;
  4. every tree vendored ON DEMAND outside thirdparty/ (the AutoRemesher/Geogram
     QuadCover solver, fetched into the gitignored examples/reference/autoremesher-src
     and statically linked into libcyber_capi and the CLI) is declared too, and every
     subtree cmake/QuadCoverSolver.cmake compiles or includes is covered by a
     declaration — a second vendored tree used to be structurally invisible here;
  5. everything marked `redistributed` is named in THIRD_PARTY_NOTICES.md, whose
     notice the MIT/BSD/MPL licenses require us to reproduce in binary form.

Exits nonzero naming each violation.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
THIRDPARTY = REPO / "thirdparty"
NOTICES = REPO / "THIRD_PARTY_NOTICES.md"
QUADCOVER_CMAKE = REPO / "cmake/QuadCoverSolver.cmake"
LICENSE_FILENAMES = {"LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "COPYING.txt"}


def _has_license_file(directory: Path, declared: str | None) -> bool:
    if declared:
        return (directory / declared).is_file()
    return any((directory / fn).is_file() for fn in LICENSE_FILENAMES)


def _check_licenses(deps: list, allowed: set, errors: list) -> None:
    for dep in deps:
        if dep["license"] not in allowed:
            errors.append(
                f"{dep['name']}: license '{dep['license']}' is not in the allowlist {sorted(allowed)}")


def _check_thirdparty(manifest: dict, errors: list) -> None:
    """Rules 1-3: the in-tree thirdparty/ directory."""
    listed_paths = set()
    for dep in manifest["dependencies"]:
        name, rel_path = dep["name"], dep["path"]
        listed_paths.add(rel_path)
        dep_dir = THIRDPARTY / rel_path
        if not dep_dir.is_dir():
            errors.append(f"{name}: declared path thirdparty/{rel_path} does not exist")
        elif not _has_license_file(dep_dir, dep.get("licenseFile")):
            errors.append(f"{name}: no license file found in thirdparty/{rel_path}")

    for entry in sorted(THIRDPARTY.iterdir()):
        if entry.is_dir() and entry.name not in listed_paths:
            errors.append(f"unlisted vendored directory: thirdparty/{entry.name} (add it to manifest.json)")


def _check_vendored_on_demand(manifest: dict, errors: list) -> None:
    """Rule 4: trees fetched on demand outside thirdparty/.

    The directory itself is only checked when it has been fetched, because a
    clean checkout has not run the CMake fetch yet. The declarations, and the
    coverage of every subtree the QuadCover build reaches, are checked always.
    """
    entries = manifest.get("vendoredOnDemand", [])
    for dep in entries:
        dep_dir = REPO / dep["path"]
        if not dep_dir.is_dir():
            continue  # not fetched in this checkout; declarations still audited
        if not _has_license_file(dep_dir, dep.get("licenseFile")):
            errors.append(f"{dep['name']}: no license file found in {dep['path']}")

    declared_subtrees = {d["quadcoverSubtree"] for d in entries if "quadcoverSubtree" in d}
    if QUADCOVER_CMAKE.is_file():
        text = QUADCOVER_CMAKE.read_text(encoding="utf-8")
        used = set(re.findall(r"thirdparty/([A-Za-z0-9_.+-]+)", text))
        for name in sorted(used - declared_subtrees):
            errors.append(
                f"cmake/QuadCoverSolver.cmake builds against vendored subtree "
                f"'{name}' that no manifest entry declares (add a vendoredOnDemand "
                f"entry with \"quadcoverSubtree\": \"{name}\")")


def _check_notices(manifest: dict, errors: list) -> None:
    """Rule 5: everything we redistribute in binary form carries its notice."""
    if not NOTICES.is_file():
        errors.append("THIRD_PARTY_NOTICES.md is missing")
        return
    notices = NOTICES.read_text(encoding="utf-8")
    shipped = [dep for dep in manifest["dependencies"] + manifest.get("vendoredOnDemand", [])
               if dep.get("redistributed")]
    for dep in shipped:
        if dep["name"] not in notices:
            errors.append(
                f"{dep['name']}: linked into shipped binaries but absent from "
                f"THIRD_PARTY_NOTICES.md ({dep['license']} requires the notice in "
                f"binary redistributions)")
    return


def main() -> int:
    manifest = json.loads((THIRDPARTY / "manifest.json").read_text())
    allowed = set(manifest["allowedLicenses"])
    errors: list[str] = []

    all_deps = manifest["dependencies"] + manifest.get("vendoredOnDemand", [])
    _check_licenses(all_deps, allowed, errors)
    _check_thirdparty(manifest, errors)
    _check_vendored_on_demand(manifest, errors)
    _check_notices(manifest, errors)

    if errors:
        print("License audit FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"License audit OK: {len(all_deps)} dependencies "
          f"({len(manifest.get('vendoredOnDemand', []))} vendored on demand), all permissive.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
