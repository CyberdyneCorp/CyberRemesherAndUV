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

It ALSO runs the other way, which is the half that was missing. Checking only
that Swift references nothing absent from the header cannot detect an entry
point no binding reaches — and that is the drift that actually accumulated: by
v0.9.0 Swift held the complete stroke grammar and none of the operations that
apply a recognised gesture, 124 entry points unbound and nothing failing. So
every declared `cyber_*` entry point must now be referenced by the Swift
sources or listed in PENDING_REGISTRATIONS below, and an unlisted, unbound
entry point fails.

It cannot prove the package compiles. It does prove the package cannot be
referencing a symbol that does not exist, calling one with the wrong arity, or
quietly leaving a capability unreachable.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import binding_parity  # noqa: E402  (path set above so the shared check imports)

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "capi" / "include" / "cyber_capi.h"
SWIFT_SOURCES = REPO / "swift" / "Sources"

# Identifiers that belong to the C ABI namespace. Anything a Swift file spells
# with one of these shapes must come from the header (or be declared in Swift).
ABI_IDENT = re.compile(r"\b(?:cyber_[A-Za-z0-9_]+|CYBER_[A-Z0-9_]+|Cyber[A-Za-z0-9_]*)\b")

# Entry points deliberately NOT bound in Swift, each with the reason.
#
# This list is the `engine-bindings` spec's "pending registration" made
# executable: a capability the ABI exposes and a binding does not is recorded
# here, so it is a decision someone made in a reviewable diff rather than an
# oversight nobody could see. Adding a line is cheap; adding one without a
# reason is what review is for.
#
# Removing an entry point from the ABI without removing it here is also caught,
# so the list cannot rot into naming symbols that no longer exist.
PENDING_REGISTRATIONS: dict[str, str] = {
    # --- superseded by a richer variant Swift already binds ----------------
    # Binding both would give a host two ways to do one thing that can drift
    # apart. The bound variant is named in each reason.
    "cyber_remesh_guided": "superseded: Swift binds cyber_remesh_guided_ex",
    "cyber_remesh_zremesher": "superseded: cyber_remesh_zremesher_with_reports",
    "cyber_remesh_zremesher_with_injectability_report": (
        "superseded: cyber_remesh_zremesher_with_reports carries both reports"
    ),
    "cyber_remesh_zremesher_with_semantic_boundary_report": (
        "superseded: cyber_remesh_zremesher_with_reports carries both reports"
    ),
    "cyber_remesh_params_default": "superseded: Swift binds cyber_default_params",
    "cyber_retopo_subdivide": "superseded: Swift binds cyber_retopo_subdivide_ex",
    "cyber_retopo_selection_transform": (
        "superseded: cyber_retopo_selection_transform_pinned with an empty pin list"
    ),
    "cyber_detect_symmetry": (
        "superseded: Swift binds _evidence and _correspondence, which are strict supersets"
    ),
    "cyber_mesh_free": "superseded: Swift binds cyber_mesh_destroy",

    # --- renderer fast paths ----------------------------------------------
    # Borrowed pointers into engine buffers, for a renderer that uploads
    # straight to the GPU. Bound when the Metal viewport stops being a
    # scaffold; a Swift host that only edits geometry uses the copying
    # accessors instead, which is the safe default for a borrowed pointer.
    "cyber_mesh_positions_ptr": "renderer fast path",
    "cyber_mesh_normals_ptr": "renderer fast path",
    "cyber_mesh_colors_ptr": "renderer fast path",
    "cyber_mesh_triangle_indices_ptr": "renderer fast path",
    "cyber_mesh_edge_indices_ptr": "renderer fast path",
    "cyber_mesh_tagged_edge_indices_ptr": "renderer fast path",
    "cyber_mesh_copy_render_positions": "renderer fast path",
    "cyber_mesh_copy_colors": "renderer fast path",
    "cyber_mesh_has_colors": "renderer fast path",

    # --- cancellable variants of bound entry points -----------------------
    # The blocking forms are bound. The cancellable ones need the same
    # RemeshOperation-style job wrapper the remesh path has, which is a
    # behaviour change rather than a binding, so it is its own piece of work.
    "cyber_uv_atlas_cancellable": "needs the cancellable-job wrapper",
    "cyber_uv_unwrap_seams_cancellable": "needs the cancellable-job wrapper",
    "cyber_bake_field": "host-implemented field callback; needs a Swift trampoline",

    # --- export bundles: desktop DCC hand-off ------------------------------
    # A sandboxed mobile host writes through its own document layer and
    # security-scoped URLs, not engine-side paths into a preset tree.
    "cyber_uv_atlas_cancellable": "finishing pipeline",
    "cyber_uv_unwrap_seams_cancellable": "finishing pipeline",
    "cyber_bake_field": "finishing pipeline",
    "cyber_export_bundle_write": "finishing pipeline",
    "cyber_default_bundle_params": "finishing pipeline",
    "cyber_bundle_result_free": "finishing pipeline",
    "cyber_bundle_result_file": "finishing pipeline",
    "cyber_bundle_result_file_encoding": "finishing pipeline",
    "cyber_bundle_result_file_padding": "finishing pipeline",
    "cyber_bundle_result_file_udim_tile": "finishing pipeline",
    "cyber_bundle_result_file_id_source": "finishing pipeline",
    "cyber_bundle_result_file_id_color": "finishing pipeline",
    "cyber_bundle_result_file_id_color_count": "finishing pipeline",
    "cyber_bundle_result_file_count": "finishing pipeline",
    "cyber_bundle_result_chart_count": "finishing pipeline",
    "cyber_bundle_result_unwrapped": "finishing pipeline",
    "cyber_bundle_result_max_angle_distortion": "finishing pipeline",
    "cyber_bundle_result_warning": "finishing pipeline",
    "cyber_bundle_result_warning_count": "finishing pipeline",
    "cyber_export_preset_builtin_count": "finishing pipeline",
    "cyber_export_preset_builtin_name": "finishing pipeline",
    "cyber_export_preset_free": "finishing pipeline",
    "cyber_export_preset_info": "finishing pipeline",
    "cyber_export_preset_map": "finishing pipeline",
    "cyber_export_preset_map_file_name": "finishing pipeline",
    "cyber_export_preset_resolve": "finishing pipeline",
    "cyber_export_preset_set_resolution": "finishing pipeline",

    # --- desktop-only surfaces --------------------------------------------
    # A sandboxed mobile host reads and writes through its own document layer
    # and security-scoped URLs, not engine-side paths.
    "cyber_mesh_load": "desktop file path",
    "cyber_mesh_save": "desktop file path",
    "cyber_handoff_open": "desktop sculpt-handoff path",
    "cyber_handoff_open_buffers": "desktop sculpt-handoff path",

    # --- CPU-only on iOS --------------------------------------------------
    # The shipped XCFramework is CPU-only by design, so a backend selector
    # would offer a mobile host a choice it does not have.
    "cyber_available_backends": "CPU-only on iOS",
    "cyber_active_backend": "CPU-only on iOS",
    "cyber_active_backend_name": "CPU-only on iOS",
    "cyber_set_backend": "CPU-only on iOS",

    # --- retopology follow-ups --------------------------------------------
    # Genuinely wanted on mobile and not yet written. Listed rather than left
    # invisible, which is the whole point of this file.
    "cyber_retopo_grow_boundary_edge": "retopology follow-up",
    "cyber_retopo_loop_subdivide": "retopology follow-up",
    "cyber_conform": "retopology follow-up",
    "cyber_remesh_with_resource_limits": "retopology follow-up",
    "cyber_remesh_zremesher_with_resource_limits": "retopology follow-up",
    "cyber_default_remesh_limits": "retopology follow-up",
    "cyber_default_remesh_execution_limits": "retopology follow-up",
    "cyber_max_worker_threads": "retopology follow-up",
}

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
    unbound, stale, redundant = binding_parity.coverage(
        sources, header_text, PENDING_REGISTRATIONS
    )

    for path, lineno, ident in missing:
        print(f"FAIL: {path.relative_to(REPO)}:{lineno}: '{ident}' is not declared in cyber_capi.h")
    for path, name, label in wrong_fields:
        print(f"FAIL: {path.relative_to(REPO)}: {name} has no field '{label}'")
    for path, lineno, name, passed, declared in wrong_arity:
        print(
            f"FAIL: {path.relative_to(REPO)}:{lineno}: {name} takes {declared} "
            f"argument(s), called with {passed}"
        )

    for line in binding_parity.report(
        "Swift", unbound, stale, redundant, PENDING_REGISTRATIONS
    ):
        print(line)

    if missing or wrong_fields or wrong_arity or unbound or stale or redundant:
        distinct = sorted({ident for _, _, ident in missing})
        print(
            f"\n{len(missing)} reference(s) to {len(distinct)} undeclared symbol(s)"
            + (f": {', '.join(distinct)}" if distinct else "")
        )
        print(f"{len(wrong_fields)} bad struct field reference(s)")
        print(f"{len(wrong_arity)} call(s) with the wrong argument count")
        print(f"{len(unbound)} unbound, unregistered entry point(s)")
        print(f"{len(stale)} stale pending registration(s)")
        print(f"{len(redundant)} registration(s) for entry points that ARE bound")
        return 1

    print(
        f"{len(sources)} Swift file(s) reference only symbols declared in cyber_capi.h, "
        f"with matching arities"
    )
    print(binding_parity.summary("Swift", header_text, PENDING_REGISTRATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
