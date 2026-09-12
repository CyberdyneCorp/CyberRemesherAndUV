#!/usr/bin/env python3
"""Exercise the C ABI manifest generator, including type-only mutations."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
GENERATOR = REPO / "tools/abi/generate_capi_manifest.py"
HEADER = REPO / "capi/include/cyber_capi.h"
PINNED = REPO / "capi/abi/cyber_capi-1.2.json"
COMPILER = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
RUNNER = os.environ.get("CYBER_TEST_LAUNCHER", "")


def generate(header: Path, output: Path) -> subprocess.CompletedProcess[str]:
    assert COMPILER is not None, "a C++ compiler is required for ABI layout validation"
    return subprocess.run([sys.executable, str(GENERATOR), "--header", str(header),
                           "--compiler", COMPILER, "--runner", RUNNER,
                           "--output", str(output)],
                          capture_output=True, text=True, check=False)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cyber-abi-manifest-") as temporary:
        root = Path(temporary)
        generated = root / "generated.json"
        result = generate(HEADER, generated)
        require(result.returncode == 0, result.stderr)
        require(json.loads(generated.read_text()) == json.loads(PINNED.read_text()),
                "the checked-in ABI manifest is stale; regenerate it deliberately")
        manifest = json.loads(generated.read_text())
        require(len(manifest["functions"]) > 200,
                "the manifest must retain the complete exported function surface")
        require(all(";" not in entry["return"] and "#" not in entry["return"]
                    for entry in manifest["functions"].values()),
                "a function declaration parser consumed unrelated header declarations")

        # Same-sized pointer swap: sizeof/offsetof-only schemes missed this.
        mutated = root / "cyber_capi.h"
        text = HEADER.read_text(encoding="utf-8")
        require("const float* points;" in text, "mutation anchor changed")
        mutated.write_text(text.replace("const float* points;", "const double* points;", 1),
                           encoding="utf-8")
        changed = root / "changed.json"
        result = generate(mutated, changed)
        require(result.returncode == 0, result.stderr)
        require(json.loads(changed.read_text()) != json.loads(PINNED.read_text()),
                "a same-layout pointer pointee mutation escaped the manifest")

        # This occupies existing trailing padding on LP64; the field list must
        # still make it a break even if sizeof(CyberBundleParams) remains unchanged.
        text = HEADER.read_text(encoding="utf-8")
        require("float aoRadius;" in text, "trailing-padding mutation anchor changed")
        mutated.write_text(text.replace("float aoRadius;", "float aoRadius;\n    int reserved;", 1),
                           encoding="utf-8")
        result = generate(mutated, changed)
        require(result.returncode == 0, result.stderr)
        require(json.loads(changed.read_text()) != json.loads(PINNED.read_text()),
                "a field added in trailing padding escaped the manifest")
    print("C ABI manifest is current and detects type/layout-only mutations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
