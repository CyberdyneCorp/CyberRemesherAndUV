#!/usr/bin/env python3
"""Swift package <-> C ABI parity guard.

The SwiftPM package in ``swift/`` binds ``capi/include/cyber_capi.h`` directly,
but no Linux CI lane can compile Swift, so the two drifted apart unnoticed
until a tagged release lane ran. This test needs NO Swift toolchain: it parses
both sides textually and asserts that

  * every ``cyber_*`` / ``CYBER_*`` / ``Cyber*`` identifier the Swift sources
    reference is actually declared by the header,
  * every ``cyber_*`` call passes as many arguments as the prototype declares,
  * every C-struct literal is built from real field names.

It cannot prove the package compiles. It does prove the package cannot be
referencing a symbol that does not exist or calling one with the wrong arity,
which is the failure mode that actually happened.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "capi" / "include" / "cyber_capi.h"
SWIFT_SOURCES = REPO / "swift" / "Sources"

# Identifiers that belong to the C ABI namespace. Anything a Swift file spells
# with one of these shapes must come from the header (or be declared in Swift).
ABI_IDENT = re.compile(r"\b(?:cyber_[A-Za-z0-9_]+|CYBER_[A-Z0-9_]+|Cyber[A-Za-z0-9_]*)\b")

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")
SWIFT_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"')

# `public final class Foo`, `struct Foo`, `extension Foo`, `enum Foo`, ...
SWIFT_DECL = re.compile(
    r"\b(?:class|struct|enum|protocol|extension|typealias|actor)\s+([A-Za-z_][A-Za-z0-9_]*)"
)

# `typedef struct CyberSeamPathOptions { ... } CyberSeamPathOptions;`
C_STRUCT = re.compile(
    r"typedef\s+struct\s+(?P<name>\w+)\s*\{(?P<body>[^{}]*)\}\s*(?P=name)\s*;", re.DOTALL
)
# One declarator of a POD field: `float flatWeight;`, `const float* points;`.
C_FIELD = re.compile(r"(?:^|;)\s*[^;{}]*?\b(\w+)\s*(?:\[[^\]]*\])?\s*(?=;)", re.MULTILINE)

# A whole prototype: `CyberStatus cyber_remesh(const CyberMesh* in, ...);`. No
# prototype in this header takes a parenthesised (function-pointer) parameter
# directly — they all go through the CyberProgressCb/CyberCancelCb typedefs —
# so a paren-free parameter list is an exact match.
C_PROTOTYPE = re.compile(r"\b(cyber_[A-Za-z0-9_]+)\s*\(([^()]*)\)\s*;", re.DOTALL)

OPENERS = {"(": ")", "[": "]", "{": "}"}
CLOSERS = {")", "]", "}"}


def strip_c_comments(text: str) -> str:
    return LINE_COMMENT.sub(" ", BLOCK_COMMENT.sub(" ", text))


def strip_swift_noise(text: str) -> str:
    """Remove comments and string literals so only real code is inspected."""
    return SWIFT_STRING.sub('""', LINE_COMMENT.sub(" ", BLOCK_COMMENT.sub(" ", text)))


def header_symbols(header_text: str) -> set[str]:
    """Every ABI-namespaced identifier the header actually declares."""
    return set(ABI_IDENT.findall(strip_c_comments(header_text)))


def header_struct_fields(header_text: str) -> dict[str, set[str]]:
    fields = {}
    for match in C_STRUCT.finditer(strip_c_comments(header_text)):
        fields[match.group("name")] = set(C_FIELD.findall(match.group("body")))
    return fields


def header_arities(header_text: str) -> dict[str, int]:
    """Declared parameter count of every `cyber_*` prototype."""
    arities = {}
    for match in C_PROTOTYPE.finditer(strip_c_comments(header_text)):
        params = match.group(2).strip()
        arities[match.group(1)] = 0 if params in ("", "void") else params.count(",") + 1
    return arities


def call_arity(code: str, open_paren: int) -> int | None:
    """Argument count of the call whose '(' sits at `open_paren`.

    Walks the bracket nesting so nested calls, array literals and closures do
    not leak their commas into the count. Returns None on an unbalanced tail.
    """
    depth = 0
    commas = 0
    saw_argument = False
    for char in code[open_paren:]:
        if char in OPENERS:
            depth += 1
        elif char in CLOSERS:
            depth -= 1
            if depth == 0:
                return commas + 1 if saw_argument else 0
        elif depth == 1:
            if char == ",":
                commas += 1
            elif not char.isspace():
                saw_argument = True
    return None


def wrong_arity_calls(
    sources: dict[Path, str], arities: dict[str, int]
) -> list[tuple[Path, int, str, int, int]]:
    """(file, line, function, passed, declared) for every mis-arity call."""
    bad = []
    for path, code in sources.items():
        for call in re.finditer(r"\b(cyber_[A-Za-z0-9_]+)\s*\(", code):
            declared = arities.get(call.group(1))
            if declared is None:
                continue  # a non-function symbol; the identifier check covers it
            passed = call_arity(code, call.end() - 1)
            if passed is not None and passed != declared:
                line = code.count("\n", 0, call.start()) + 1
                bad.append((path, line, call.group(1), passed, declared))
    return bad


def swift_files() -> list[Path]:
    return sorted(SWIFT_SOURCES.rglob("*.swift"))


def swift_declared_types(sources: dict[Path, str]) -> set[str]:
    declared = set()
    for code in sources.values():
        declared.update(SWIFT_DECL.findall(code))
    return declared


def undeclared_symbol_uses(
    sources: dict[Path, str], known: set[str], swift_types: set[str]
) -> list[tuple[Path, int, str]]:
    """(file, line, identifier) for every ABI identifier the header lacks."""
    bad = []
    for path, code in sources.items():
        for lineno, line in enumerate(code.splitlines(), start=1):
            for ident in ABI_IDENT.findall(line):
                if ident not in known and ident not in swift_types:
                    bad.append((path, lineno, ident))
    return bad


def balanced_args(code: str, open_index: int) -> str | None:
    """The argument text of the parenthesised group starting at `open_index`.

    A regex cannot do this: `[^()]*` stops at the first nested paren, so any
    literal containing a call — `Int32(clamping: n)` — was skipped entirely and
    never checked. Depth counting reads the whole literal instead.
    """
    depth = 0
    for i in range(open_index, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return code[open_index + 1:i]
    return None  # unbalanced (truncated source); nothing to check


def argument_labels(args: str) -> list[str]:
    """Top-level `label:` names in a Swift argument list.

    Nested groups are skipped so an inner call's labels are not attributed to
    the outer struct, and a label must start like an identifier: the `1` in a
    ternary `flag ? 1 : 0` is not a field name, and reporting it as one was a
    false positive that failed any literal built from a Bool.
    """
    labels = []
    depth = 0
    token = ""
    for ch in args:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and ch == ":":
            candidate = token.strip().split()[-1] if token.strip() else ""
            if re.fullmatch(r"[A-Za-z_]\w*", candidate):
                labels.append(candidate)
            token = ""
            continue
        elif depth == 0 and ch == ",":
            token = ""
            continue
        if depth == 0:
            token += ch
    return labels


def bad_struct_fields(
    sources: dict[Path, str], struct_fields: dict[str, set[str]]
) -> list[tuple[Path, str, str]]:
    """(file, struct, label) for C-struct literals built with unknown fields."""
    bad = []
    for path, code in sources.items():
        for name, fields in struct_fields.items():
            for match in re.finditer(re.escape(name) + r"\s*\(", code):
                args = balanced_args(code, match.end() - 1)
                if args is None:
                    continue
                for label in argument_labels(args):
                    if label not in fields:
                        bad.append((path, name, label))
    return bad


def main() -> int:
    if not HEADER.is_file():
        print(f"FAIL: missing header {HEADER}")
        return 1
    sources = {path: strip_swift_noise(path.read_text()) for path in swift_files()}
    if not sources:
        print(f"FAIL: no Swift sources under {SWIFT_SOURCES}")
        return 1

    header_text = HEADER.read_text()
    known = header_symbols(header_text)
    structs = header_struct_fields(header_text)
    swift_types = swift_declared_types(sources)

    missing = undeclared_symbol_uses(sources, known, swift_types)
    wrong_fields = bad_struct_fields(sources, structs)
    wrong_arity = wrong_arity_calls(sources, header_arities(header_text))

    for path, lineno, ident in missing:
        print(f"FAIL: {path.relative_to(REPO)}:{lineno}: '{ident}' is not declared in cyber_capi.h")
    for path, name, label in wrong_fields:
        print(f"FAIL: {path.relative_to(REPO)}: {name} has no field '{label}'")
    for path, lineno, name, passed, declared in wrong_arity:
        print(
            f"FAIL: {path.relative_to(REPO)}:{lineno}: {name} takes {declared} "
            f"argument(s), called with {passed}"
        )

    if missing or wrong_fields or wrong_arity:
        distinct = sorted({ident for _, _, ident in missing})
        print(
            f"\n{len(missing)} reference(s) to {len(distinct)} undeclared symbol(s)"
            + (f": {', '.join(distinct)}" if distinct else "")
        )
        print(f"{len(wrong_fields)} bad struct field reference(s)")
        print(f"{len(wrong_arity)} call(s) with the wrong argument count")
        return 1

    print(
        f"Swift/C ABI parity OK: {len(sources)} Swift file(s) reference only "
        f"symbols declared in cyber_capi.h, with matching arities"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
