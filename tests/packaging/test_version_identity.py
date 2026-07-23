#!/usr/bin/env python3
"""Single version identity (build-and-packaging spec).

The spec requires the semantic version to originate from one source of truth
"with no possibility of divergence". The C++ side already satisfies that: the
root `project(... VERSION ...)` field flows into `CYBER_VERSION_STRING`, the C
ABI version macros, and the packaging artifact names.

The Python packages do not — each hardcodes its own literal. Nothing linked
them, so a release bump that missed one would ship a `cyberremesh` wheel
claiming a version the engine inside it disagrees with. This test is the link.

Pure file parsing: no engine build, no import of the packages themselves (they
would need the shared library), so it always runs rather than skipping.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Every file that states the project version, and how to read it. Kept explicit
# so a reviewer can see the full set; `test_no_undeclared_version` below guards
# against a new one being added without landing here.
DECLARATIONS = [
    ("CMakeLists.txt", re.compile(r"^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$", re.M)),
    ("python/pyproject.toml", re.compile(r'^version\s*=\s*"(\d+\.\d+\.\d+)"', re.M)),
    ("python/cyberremesh/pyproject.toml", re.compile(r'^version\s*=\s*"(\d+\.\d+\.\d+)"', re.M)),
    ("python/cyberbridge/__init__.py", re.compile(r'^__version__\s*=\s*"(\d+\.\d+\.\d+)"', re.M)),
    ("python/cyberremesh/cyberremesh/__init__.py",
     re.compile(r'^__version__\s*=\s*"(\d+\.\d+\.\d+)"', re.M)),
]

FAILURES: "list[str]" = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def read_versions() -> "dict[str, str]":
    found = {}
    for rel, pattern in DECLARATIONS:
        path = REPO / rel
        if not path.is_file():
            check(f"{rel} exists", False, "declared source of version is missing")
            continue
        m = pattern.search(path.read_text(encoding="utf-8"))
        if m is None:
            check(f"{rel} states a version", False, f"no match for {pattern.pattern!r}")
            continue
        found[rel] = m.group(1)
    return found


def test_all_declarations_agree(found: "dict[str, str]") -> None:
    """The whole point: one version, everywhere."""
    authority = found.get("CMakeLists.txt")
    check("CMakeLists.txt is readable as the source of truth", authority is not None)
    if authority is None:
        return
    for rel, version in found.items():
        check(f"{rel} == {authority}", version == authority,
              f"reads {version}, but CMakeLists.txt (source of truth) reads {authority}")


def test_no_undeclared_version() -> None:
    """Catch a version literal added somewhere DECLARATIONS does not cover.

    Without this, the test above stays green while a new package quietly
    diverges — which is the exact failure mode it exists to prevent.
    """
    pattern = re.compile(r'^(?:__version__|version)\s*=\s*"(\d+\.\d+\.\d+)"', re.M)
    known = {rel for rel, _ in DECLARATIONS}
    stray = []
    for path in sorted((REPO / "python").rglob("*")):
        if path.suffix not in (".py", ".toml") or not path.is_file():
            continue
        # Build outputs and vendored trees are not ours to keep in sync.
        rel = path.relative_to(REPO).as_posix()
        if any(part in rel for part in ("build", ".egg-info", "site-packages", "__pycache__")):
            continue
        if rel in known:
            continue
        if pattern.search(path.read_text(encoding="utf-8", errors="ignore")):
            stray.append(rel)
    check("no version literal outside DECLARATIONS", not stray,
          f"unlinked version(s) in: {', '.join(stray)}")


def main() -> int:
    print(f"version identity (repo: {REPO})")
    found = read_versions()
    for rel, version in found.items():
        print(f"  {rel}: {version}")
    test_all_declarations_agree(found)
    test_no_undeclared_version()

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("\nall version declarations agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
