#!/usr/bin/env python3
"""License audit gate (build-and-packaging spec, "Permissive-license dependency policy").

Checks that:
  1. every dependency in thirdparty/manifest.json declares an allowed license;
  2. every directory under thirdparty/ is listed in the manifest (no stray vendored code);
  3. every listed dependency directory exists and contains a license file.

Exits nonzero naming each violation.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
THIRDPARTY = REPO / "thirdparty"
LICENSE_FILENAMES = {"LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "COPYING.txt"}


def main() -> int:
    manifest = json.loads((THIRDPARTY / "manifest.json").read_text())
    allowed = set(manifest["allowedLicenses"])
    errors: list[str] = []

    listed_paths = set()
    for dep in manifest["dependencies"]:
        name, license_id, rel_path = dep["name"], dep["license"], dep["path"]
        listed_paths.add(rel_path)
        if license_id not in allowed:
            errors.append(f"{name}: license '{license_id}' is not in the allowlist {sorted(allowed)}")
        dep_dir = THIRDPARTY / rel_path
        if not dep_dir.is_dir():
            errors.append(f"{name}: declared path thirdparty/{rel_path} does not exist")
        elif not any((dep_dir / fn).is_file() for fn in LICENSE_FILENAMES):
            errors.append(f"{name}: no license file found in thirdparty/{rel_path}")

    for entry in sorted(THIRDPARTY.iterdir()):
        if entry.is_dir() and entry.name not in listed_paths:
            errors.append(f"unlisted vendored directory: thirdparty/{entry.name} (add it to manifest.json)")

    if errors:
        print("License audit FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"License audit OK: {len(manifest['dependencies'])} dependencies, all permissive.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
