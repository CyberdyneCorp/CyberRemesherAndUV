#!/usr/bin/env python3
"""Objective-C++ syntax gate for the Metal compute backend, off Apple hardware.

``src/accel/src/metal_backend.mm`` is the one source file in this repository
that no lane could feed to any compiler: it needs an Apple toolchain, and the
only job that has one (ci.yml's ``gpu-metal-compile``) runs
``continue-on-error``. So the file was unguarded — a selector typo, a wrong
argument type or a stale signature could sit in it indefinitely.

Clang parses Objective-C++ on Linux perfectly well; what it lacks is
Metal.framework and Foundation.framework. ``tests/packaging/objcxx_stubs/``
supplies declaration-only headers with the SDK's exact signatures, and this
test compiles the backend against them with ``-fsyntax-only`` under
``-Wall -Wextra -Werror`` and ARC.

What it proves: the file parses, every selector it sends exists with the
argument and return types the SDK declares, and it is ARC-clean. What it does
NOT prove: that it links, that Metal accepts the MSL kernel source, or that the
results are right. Those need Apple hardware — ci.yml's ``gpu-metal-compile``
and hardening.yml's ``gpu-parity``.

The gate also mutation-checks ITSELF: after the real file passes, a copy with a
deliberately mistyped selector must FAIL. A stub that silently stopped
declaring anything (or a clang that stopped diagnosing unknown selectors) would
otherwise turn this into a test that passes no matter what the file says.

Exits 77 (CTest's SKIP code) when no clang that can do this is installed.
"""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "src" / "accel" / "src" / "metal_backend.mm"
STUBS = Path(__file__).resolve().parent / "objcxx_stubs"

SKIP = 77

# -fobjc-runtime=ios-13.0 is what makes a non-Apple clang parse the modern
# dialect (ARC, properties, instancetype) the backend and the SDK are written
# in; the default GNUstep runtime rejects ARC outright.
FLAGS = [
    "-x",
    "objective-c++",
    "-std=c++20",
    "-fsyntax-only",
    "-fobjc-arc",
    "-fobjc-runtime=ios-13.0",
    "-Wall",
    "-Wextra",
    "-Werror",
    f"-I{STUBS}",
    f"-I{REPO / 'src' / 'accel' / 'include'}",
    f"-I{REPO / 'src' / 'core' / 'include'}",
]

# A selector the file really sends, and a typo of it. The mutant must be
# rejected; if the file stops sending this one, the pattern stops matching and
# the mutation check says so rather than passing vacuously.
MUTATION = (re.compile(r"\bendEncoding\]"), "endEncodingNoSuchSelector]")


def find_clang() -> str | None:
    for name in ("clang++", "clang++-18", "clang++-17", "clang++-16", "clang"):
        path = shutil.which(name)
        if path is None:
            continue
        probe = subprocess.run(
            [path, *FLAGS, "-"], input="int main(){return 0;}\n", capture_output=True, text=True
        )
        if probe.returncode == 0:
            return path
    return None


def syntax_check(clang: str, source: Path) -> subprocess.CompletedProcess:
    return subprocess.run([clang, *FLAGS, str(source)], capture_output=True, text=True)


def main() -> int:
    if not SOURCE.is_file():
        print(f"FAIL: {SOURCE} is missing", file=sys.stderr)
        return 1

    clang = find_clang()
    if clang is None:
        print("SKIP: no clang that parses Objective-C++ with -fobjc-runtime=ios-13.0")
        return SKIP
    print(f"using {clang}")

    result = syntax_check(clang, SOURCE)
    if result.returncode != 0:
        print("FAIL: the Metal backend does not pass the Objective-C++ syntax gate.")
        print("      Either the file is wrong, or it calls a symbol the stub headers in")
        print(f"      {STUBS} do not declare yet — add it with the SDK's exact signature.")
        print(result.stderr, file=sys.stderr)
        return 1
    print("  ok: metal_backend.mm parses clean under -Wall -Wextra -Werror with ARC")

    pattern, replacement = MUTATION
    text = SOURCE.read_text()
    mutant, count = pattern.subn(replacement, text, count=1)
    if count != 1:
        print(f"FAIL: the mutation check's anchor {pattern.pattern!r} is no longer in the file;")
        print("      point MUTATION at a selector the backend really sends.", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "metal_backend_mutant.mm"
        path.write_text(mutant)
        if syntax_check(clang, path).returncode == 0:
            print("FAIL: the gate accepted a mistyped selector, so it proves nothing.")
            print("      The stub headers are too permissive.", file=sys.stderr)
            return 1
    print("  ok: a mistyped selector is rejected (the gate is not vacuous)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
