#!/usr/bin/env python3
"""Build hygiene regressions (build-and-packaging spec).

Three defects this guards, each of which was invisible to the C++ suite because
it lives in the build description rather than in code:

* `-Werror` on by default used to be unbuildable on GCC 13 (a libstdc++
  `-Wstringop-overflow` false positive from one `vector::insert`), and the
  escape hatch was to switch warning enforcement off for the whole tree. The
  fix is a compiler-guarded pragma at that single site, so this test pins both
  halves: the default stays ON and the suppression stays narrow.
* `-DCYBER_BUILD_RETOPO=OFF` used to configure cleanly and then die at the very
  end with `cannot find -lcyber_retopo`, because capi links a target that was
  never added.
* the shared C ABI used to export ~4000 vendored Geogram/stb symbols, which a
  host process carrying its own copy can interpose on. Export control has to
  exist on all three platforms and the binary check has to work on all three
  object formats: `nm -D` is ELF-only, so the macOS export list was asserted by
  nothing at all, and Windows had no mechanism whatsoever.
* the module CMakeLists used to reach the vendored headers through
  `${CMAKE_SOURCE_DIR}`, which under `add_subdirectory()` points at the
  CONSUMER's source root, so every include path was wrong and nothing compiled.

Mostly file parsing so it runs everywhere; the configure and exported-symbol
checks skip themselves when cmake / nm / the built library are unavailable.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Trees that are not ours to police: vendored sources, build outputs, and the
# Android shell, which declares its own project() and is configured standalone.
SKIPPED_DIRS = ("thirdparty", "build", "examples/reference", "apps/mobile")

FAILURES: "list[str]" = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def skip(name: str, reason: str) -> None:
    print(f"  skip: {name} ({reason})")


def test_warnings_as_errors_default_on() -> None:
    """The default configuration must enforce warnings, not opt out of them."""
    text = (REPO / "cmake/CompilerWarnings.cmake").read_text(encoding="utf-8")
    m = re.search(r'option\(CYBER_WARNINGS_AS_ERRORS\s+"[^"]*"\s+(\w+)\)', text)
    check("CYBER_WARNINGS_AS_ERRORS is declared", m is not None)
    if m is not None:
        check("CYBER_WARNINGS_AS_ERRORS defaults to ON", m.group(1) == "ON",
              f"defaults to {m.group(1)}")
    check("no tree-wide -Wno- suppression", "-Wno-" not in text,
          "a blanket -Wno- flag hides the diagnostic everywhere, not at its site")


def test_stringop_overflow_suppressed_at_its_site() -> None:
    """The GCC 12/13 false positive is silenced narrowly, guarded by compiler."""
    path = REPO / "src/quadrangulate/src/sparse_cholesky.cpp"
    text = path.read_text(encoding="utf-8")
    check("sparse_cholesky.cpp ignores -Wstringop-overflow",
          '#pragma GCC diagnostic ignored "-Wstringop-overflow"' in text)
    check("the suppression is guarded on GCC",
          "defined(__GNUC__) && !defined(__clang__)" in text,
          "an unguarded pragma also hides the warning on other toolchains")
    check("the suppression is pushed and popped",
          text.count("#pragma GCC diagnostic push") == text.count(
              "#pragma GCC diagnostic pop") == 1,
          "an unpopped push leaks the suppression to the rest of the file")
    # Narrow means the pop follows within a couple of lines of the insert.
    lines = text.splitlines()
    insert = [i for i, ln in enumerate(lines) if "queue.insert(queue.end()" in ln]
    check("the guarded region covers the range insert", len(insert) == 1)
    if len(insert) == 1:
        window = "\n".join(lines[insert[0]:insert[0] + 4])
        check("the suppression ends right after the range insert",
              "#pragma GCC diagnostic pop" in window)


def test_capi_requires_retopo_at_configure_time() -> None:
    """-DCYBER_BUILD_RETOPO=OFF must fail loudly at configure, not at link."""
    cmake = shutil.which("cmake")
    if cmake is None:
        skip("CYBER_BUILD_RETOPO=OFF is rejected", "cmake not on PATH")
        return
    with tempfile.TemporaryDirectory() as tmp:
        proc = subprocess.run(
            [cmake, "-S", str(REPO), "-B", tmp, "-DCYBER_BUILD_RETOPO=OFF",
             "-DCYBER_BUILD_TESTS=OFF", "-DCYBER_BUILD_CLI=OFF"],
            capture_output=True, text=True, check=False)
    output = proc.stdout + proc.stderr
    if proc.returncode == 0:
        # The regression itself: configure passes and the build dies much later
        # on `cannot find -lcyber_retopo`.
        check("CYBER_BUILD_RETOPO=OFF is rejected", False,
              "configure succeeded; the missing target becomes a raw -lcyber_retopo")
    elif "CYBER_BUILD_CAPI requires CYBER_BUILD_RETOPO" in output:
        check("CYBER_BUILD_RETOPO=OFF is rejected", True)
    else:
        # Configure died for some other reason (no default generator, no
        # compiler); that says nothing about the option dependency.
        skip("CYBER_BUILD_RETOPO=OFF is rejected",
             f"configure failed before the option check: {output.strip()[-200:]}")


def test_export_control_exists_on_every_platform() -> None:
    """Each of the three platforms must control what the shared C ABI exports.

    ELF and Mach-O narrow the export set to `cyber_*`; PE cannot be interposed
    per-symbol the way ELF's flat namespace allows, so there the requirement is
    only that SOMETHING exports the ABI — MSVC exports nothing at all without it,
    which yields an empty export table and no import library.
    """
    cmake = (REPO / "capi/CMakeLists.txt").read_text(encoding="utf-8")

    version_script = REPO / "capi/cyber_capi.map"
    check("capi/cyber_capi.map exists", version_script.is_file())
    if version_script.is_file():
        text = version_script.read_text(encoding="utf-8")
        check("the version script exports cyber_* only",
              "cyber_*;" in text and re.search(r"local:\s*\*;", text) is not None)
    check("the shared target applies the version script (ELF)",
          "--version-script" in cmake)

    # The Mach-O export list was added with the version script but nothing ever
    # asserted it, so an edit that re-exported the vendored Geogram symbols into
    # a DCC host process would have gone unnoticed on macOS.
    symbols = REPO / "capi/cyber_capi.symbols"
    check("capi/cyber_capi.symbols exists", symbols.is_file())
    if symbols.is_file():
        listed = [ln.strip() for ln in symbols.read_text(encoding="utf-8").splitlines()
                  if ln.strip() and not ln.startswith("#")]
        check("the Mach-O list exports _cyber_* only",
              listed == ["_cyber_*"], f"lists {listed}")
    check("the shared target applies the Mach-O export list",
          "-exported_symbols_list" in cmake)

    check("the shared target controls exports on Windows",
          "WINDOWS_EXPORT_ALL_SYMBOLS" in cmake or "dllexport" in cmake or ".def" in cmake,
          "MSVC exports nothing without one: empty export table, no import library")


def _exported_symbols(nm: str, lib: str) -> "tuple[list[str], str] | None":
    """(symbol names, object format) for `lib`, or None when nm cannot read it.

    `nm -D` is ELF-only: on Mach-O llvm-nm reports "no dynamic symbol table" and
    exits nonzero, and cctools nm rejects -D outright, so the ELF invocation used
    to make the whole check skip itself on macOS.
    """
    attempts = (
        ([nm, "-D", "--defined-only", lib], "elf"),
        ([nm, "-gU", lib], "macho"),          # global, defined; Mach-O prefixes with _
    )
    for argv, fmt in attempts:
        proc = subprocess.run(argv, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            continue
        names = [ln.split()[-1] for ln in proc.stdout.splitlines() if ln.strip()]
        if fmt == "macho":
            names = [n[1:] if n.startswith("_") else n for n in names]
        if names:
            return names, fmt
    return None


def test_capi_shared_exports_only_the_c_abi() -> None:
    """The built library must not export the statically linked third-party symbols."""
    name = "libcyber_capi exports only cyber_*"
    lib = os.environ.get("CYBER_CAPI_LIB")
    nm = shutil.which("nm")
    if not lib or not Path(lib).is_file() or nm is None:
        skip(name, "built library or nm unavailable")
        return
    if lib.lower().endswith(".dll"):
        # WINDOWS_EXPORT_ALL_SYMBOLS exports everything by design (see above), so
        # only the presence of the ABI is meaningful here.
        found = _exported_symbols(nm, lib)
        check("cyber_capi.dll exports the C ABI",
              found is not None and any(s.startswith("cyber_") for s in found[0]),
              "empty export table — no dllexport and no WINDOWS_EXPORT_ALL_SYMBOLS")
        return

    found = _exported_symbols(nm, lib)
    if found is None:
        skip(name, "nm could not read the library in any supported format")
        return
    exported, fmt = found
    stray = sorted({s for s in exported if not s.startswith("cyber_")})
    check(f"{name} ({fmt})", not stray,
          f"{len(stray)} foreign symbol(s), e.g. {', '.join(stray[:5])}")
    check("libcyber_capi still exports the C ABI", len(exported) > 0,
          "nothing exported at all — the export list is too strict")


def _project_cmake_files() -> "list[Path]":
    """Every CMake script this project owns, vendored trees excluded."""
    found = (list(REPO.rglob("CMakeLists.txt")) + list(REPO.rglob("*.cmake"))
             + list(REPO.rglob("*.cmake.in")))
    ours = []
    for path in sorted(found):
        rel = path.relative_to(REPO).as_posix()
        if any(rel == d or rel.startswith(d + "/") for d in SKIPPED_DIRS):
            continue
        ours.append(path)
    return ours


def test_no_root_scoped_path_variables() -> None:
    """Modules must locate this tree via PROJECT_*, never CMAKE_SOURCE_DIR.

    Under `add_subdirectory()` CMAKE_SOURCE_DIR / CMAKE_BINARY_DIR are the
    CONSUMER's roots, so every path built from them lands outside this repo.
    """
    offenders = []
    for path in _project_cmake_files():
        if path == REPO / "CMakeLists.txt":
            continue  # the top-level file is the one place they are equivalent
        text = path.read_text(encoding="utf-8")
        for var in ("CMAKE_SOURCE_DIR", "CMAKE_BINARY_DIR"):
            if "${" + var + "}" in text:
                offenders.append(f"{path.relative_to(REPO)}:{var}")
    check("no module uses CMAKE_SOURCE_DIR/CMAKE_BINARY_DIR", not offenders,
          f"use PROJECT_/CMAKE_CURRENT_ instead: {', '.join(offenders)}")


def test_no_orphaned_module_sources() -> None:
    """Every source under src/ must be named by some CMakeLists.

    A translation unit no target compiles is invisible to the whole suite: it
    can name headers that no longer exist, and it can carry policy that reads
    as live (`src/core/src/quad/quadcover.cpp` installed a process-global
    Geogram logger setting that host-hygiene work had removed everywhere else,
    while including a `quad/stages.hpp` that had already been deleted). Nothing
    fails until someone adds the file back to a build.
    """
    referenced = "\n".join(p.read_text(encoding="utf-8") for p in _project_cmake_files())
    orphans = []
    for pattern in ("src/**/*.cpp", "src/**/*.mm", "src/**/*.cu"):
        for path in sorted(REPO.glob(pattern)):
            if path.name not in referenced:
                orphans.append(path.relative_to(REPO).as_posix())
    check("no orphaned source under src/", not orphans,
          f"not compiled by any target: {', '.join(orphans)}")


def test_add_subdirectory_consumption_resolves_paths() -> None:
    """A parent project embedding this tree must get valid include paths.

    Configure only — the regression is visible in compile_commands.json, where
    the vendored header dirs pointed at the parent's source root and so did not
    exist. Building the whole tree here would be far too slow.
    """
    name = "add_subdirectory include paths exist"
    cmake = shutil.which("cmake")
    if cmake is None:
        skip(name, "cmake not on PATH")
        return
    with tempfile.TemporaryDirectory() as tmp:
        parent = Path(tmp) / "parent"
        parent.mkdir()
        (parent / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(CyberConsumerProbe LANGUAGES CXX)\n"
            f"add_subdirectory({REPO.as_posix()} cyberremesher)\n",
            encoding="utf-8")
        build = Path(tmp) / "b"
        proc = subprocess.run(
            [cmake, "-S", str(parent), "-B", str(build),
             "-DCYBER_BUILD_TESTS=ON", "-DCYBER_WITH_QUADCOVER=OFF"],
            capture_output=True, text=True, check=False)
        db = build / "compile_commands.json"
        if proc.returncode != 0 or not db.is_file():
            output = (proc.stdout + proc.stderr).strip()[-200:]
            skip(name, f"parent configure produced no compile database: {output}")
            return
        entries = json.loads(db.read_text(encoding="utf-8"))

    missing = set()
    for entry in entries:
        command = entry.get("command") or " ".join(entry.get("arguments", []))
        for inc in re.findall(r"-(?:isystem|I)\s*(\S+)", command):
            if not Path(inc).is_dir():
                missing.add(inc)
    check(name, not missing,
          f"{len(missing)} include dir(s) outside this tree, "
          f"e.g. {', '.join(sorted(missing)[:3])}")


def main() -> int:
    print(f"build hygiene (repo: {REPO})")
    test_warnings_as_errors_default_on()
    test_stringop_overflow_suppressed_at_its_site()
    test_capi_requires_retopo_at_configure_time()
    test_export_control_exists_on_every_platform()
    test_capi_shared_exports_only_the_c_abi()
    test_no_root_scoped_path_variables()
    test_no_orphaned_module_sources()
    test_add_subdirectory_consumption_resolves_paths()

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("\nbuild hygiene checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
