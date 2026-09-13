#!/usr/bin/env python3
"""Generate a type-aware, compiler-measured manifest for cyber_capi.h.

The source spelling records identities that a byte-layout check cannot see
(for example `float *` versus `double *`).  A tiny program compiled with the
selected C++ compiler records the platform ABI layout, including field offsets,
alignment and trailing padding.  Keeping both in one checked-in document makes
an ABI review an explicit versioning decision instead of a visual header diff.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)


def normalized(text: str) -> str:
    return " ".join(text.replace("\n", " ").split())


def declarations(text: str, pattern: str) -> list[tuple[str, str]]:
    return [(name, body) for name, body in re.findall(pattern, text, re.DOTALL)]


def field_name(declaration: str) -> str:
    match = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^]]*\])?\s*$", declaration)
    if match is None:
        raise ValueError(f"cannot find field name in {declaration!r}")
    return match.group(1)


def parameter_type(parameter: str) -> str:
    parameter = normalized(parameter)
    if parameter in ("", "void", "..."):
        return parameter
    # C parameter names are not part of the calling convention.  This keeps a
    # documentation-only rename from looking like an ABI break while retaining
    # pointers, fixed arrays, qualifiers and all named types.
    return re.sub(r"\s+[A-Za-z_]\w*(?=\s*(?:\[[^]]*\])?$)", "", parameter)


def parse_header(header: Path) -> dict:
    text = strip_comments(header.read_text(encoding="utf-8"))
    structs: dict[str, dict] = {}
    for name, body in declarations(text, r"typedef\s+struct\s+(Cyber\w+)\s*\{(.*?)\}\s*\1\s*;"):
        fields = []
        for raw in body.split(";"):
            declaration = normalized(raw)
            if declaration:
                fields.append({"name": field_name(declaration), "type": declaration})
        structs[name] = {"fields": fields}

    enums: dict[str, dict] = {}
    for name, body in declarations(text, r"typedef\s+enum\s+(Cyber\w+)\s*\{(.*?)\}\s*\1\s*;"):
        values = []
        for raw in body.split(","):
            entry = normalized(raw)
            if not entry:
                continue
            parts = [normalized(part) for part in entry.split("=", 1)]
            values.append({"name": parts[0], "value": parts[1] if len(parts) == 2 else None})
        enums[name] = {"values": values}

    callbacks = {}
    for ret, name, params in re.findall(
        r"typedef\s+(.+?)\s*\(\s*\*\s*(Cyber\w+)\s*\)\s*\((.*?)\)\s*;", text, re.DOTALL
    ):
        callbacks[name] = {
            "return": normalized(ret),
            "parameters": [parameter_type(item) for item in params.split(",")],
        }

    without_aggregates = re.sub(
        r"typedef\s+(?:struct|enum)\s+Cyber\w+\s*\{.*?\}\s*Cyber\w+\s*;", "", text, flags=re.DOTALL
    )
    functions = {}
    for ret, name, params in re.findall(
        r"(?:^|\n)[ \t]*([A-Za-z_][\w \t\*]*?)[ \t]+(cyber_\w+)\s*\((.*?)\)\s*;",
        without_aggregates, re.DOTALL
    ):
        functions[name] = {
            "return": normalized(ret),
            "parameters": [parameter_type(item) for item in params.split(",")],
        }

    macros = {}
    for name, value in re.findall(r"^\s*#define\s+(CYBER_ABI_VERSION_\w+)\s+([^\s]+)", text, re.MULTILINE):
        macros[name] = value
    return {"macros": macros, "structs": structs, "enums": enums,
            "callbacks": callbacks, "functions": functions}


def probe_source(parsed: dict) -> str:
    lines = ["#include <stddef.h>", "#include <stdio.h>", '#include "cyber_capi.h"', "int main() {"]
    for name, info in parsed["structs"].items():
        lines.append(f'  printf("S\\t{name}\\t%zu\\t%zu\\n", sizeof({name}), alignof({name}));')
        for field in info["fields"]:
            field_name_ = field["name"]
            lines.append(
                f'  printf("F\\t{name}\\t{field_name_}\\t%zu\\t%zu\\n", '
                f'offsetof({name}, {field_name_}), sizeof((({name}*)0)->{field_name_}));')
    for name in parsed["enums"]:
        lines.append(
            f'  printf("E\\t{name}\\t%zu\\t%zu\\t%d\\n", sizeof({name}), alignof({name}), '
            f'static_cast<{name}>(-1) < static_cast<{name}>(0) ? 1 : 0);')
    lines.extend(["  return 0;", "}"])
    return "\n".join(lines) + "\n"


def measured_layout(parsed: dict, header: Path, compiler: str, runner: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cyber-abi-") as temporary:
        root = Path(temporary)
        source = root / "probe.cpp"
        binary = root / "probe"
        source.write_text(probe_source(parsed), encoding="utf-8")
        compiler_args = [compiler, "-std=c++20", "-I", str(header.parent)]
        # CMake normally supplies this for AppleClang.  The standalone probe
        # invokes the compiler directly, so preserve the same SDK discovery.
        if sys.platform == "darwin":
            sdk = subprocess.run(["xcrun", "--show-sdk-path"], check=True,
                                 capture_output=True, text=True).stdout.strip()
            compiler_args.extend(["-isysroot", sdk])
        compile_result = subprocess.run(
            [*compiler_args, str(source), "-o", str(binary)],
            check=False, capture_output=True, text=True)
        if compile_result.returncode:
            raise RuntimeError(f"ABI layout probe did not compile:\n{compile_result.stderr}")
        output = subprocess.run([*shlex.split(runner), str(binary)], check=True,
                                capture_output=True, text=True).stdout
    for line in output.splitlines():
        kind, name, *values = line.split("\t")
        if kind == "S":
            parsed["structs"][name].update({"size": int(values[0]), "alignment": int(values[1])})
        elif kind == "F":
            field = next(item for item in parsed["structs"][name]["fields"] if item["name"] == values[0])
            field.update({"offset": int(values[1]), "size": int(values[2])})
        elif kind == "E":
            parsed["enums"][name].update({"size": int(values[0]), "alignment": int(values[1]),
                                          "signed": bool(int(values[2]))})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--runner", default="",
                        help="optional emulator command for a cross-compiled probe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    header = args.header.resolve()
    manifest = parse_header(header)
    measured_layout(manifest, header, args.compiler, args.runner)
    manifest["schema"] = 1
    manifest["header"] = header.name
    manifest["compiler"] = "toolchain-measured (compiler identity intentionally not pinned)"
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
