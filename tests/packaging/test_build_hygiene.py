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
  host process carrying its own copy can interpose on.

Mostly file parsing so it runs everywhere; the configure and exported-symbol
checks skip themselves when cmake / nm / the built library are unavailable.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

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


def test_capi_shared_exports_only_the_c_abi() -> None:
    """The .so must not export the statically linked third-party symbols."""
    script = REPO / "capi/cyber_capi.map"
    check("capi/cyber_capi.map exists", script.is_file())
    if script.is_file():
        text = script.read_text(encoding="utf-8")
        check("the version script exports cyber_* only",
              "cyber_*;" in text and re.search(r"local:\s*\*;", text) is not None)
    check("the shared target applies the version script",
          "--version-script" in (REPO / "capi/CMakeLists.txt").read_text(encoding="utf-8"))

    lib = os.environ.get("CYBER_CAPI_LIB")
    nm = shutil.which("nm")
    if not lib or not Path(lib).is_file() or nm is None:
        skip("libcyber_capi exports only cyber_*", "built library or nm unavailable")
        return
    proc = subprocess.run([nm, "-D", "--defined-only", lib],
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        skip("libcyber_capi exports only cyber_*", "nm could not read the library")
        return
    exported = [ln.split()[-1] for ln in proc.stdout.splitlines() if ln.strip()]
    stray = sorted({s for s in exported if not s.startswith("cyber_")})
    check("libcyber_capi exports only cyber_*", not stray,
          f"{len(stray)} foreign symbol(s), e.g. {', '.join(stray[:5])}")
    check("libcyber_capi still exports the C ABI", len(exported) > 0,
          "nothing exported at all — the version script is too strict")


def main() -> int:
    print(f"build hygiene (repo: {REPO})")
    test_warnings_as_errors_default_on()
    test_stringop_overflow_suppressed_at_its_site()
    test_capi_requires_retopo_at_configure_time()
    test_capi_shared_exports_only_the_c_abi()

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("\nbuild hygiene checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
