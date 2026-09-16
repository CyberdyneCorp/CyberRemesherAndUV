"""Shared header -> binding coverage check.

Every binding over the C ABI has the same failure mode, and only the Swift gate
ever checked for it: an entry point nobody bound is invisible to a test that
only asks "does every symbol this binding names exist in the header". By v0.9.0
that had let 124 entry points go unbound in Swift and 71 in Python, with both
lanes green.

So this is the direction that matters, factored out once: every `cyber_*`
function the header declares must be referenced by the binding's sources, or
listed in that binding's PENDING_REGISTRATIONS with a reason. A registration
naming a symbol the header no longer declares fails too, so the list cannot rot
into naming things that stopped existing.

The two bindings keep their own forward checks, because "referencing something
that does not exist" means different things in Swift (a compile error the CI
lane would eventually catch) and in ctypes (an AttributeError at first call,
possibly in a user's process).
"""

from __future__ import annotations

import re

DECLARED = re.compile(r"\b(cyber_[a-z0-9_]+)\s*\(")
REFERENCED = re.compile(r"\b(cyber_[a-z0-9_]+)\b")


def declared_entry_points(header_text: str) -> set[str]:
    """Every `cyber_*` function the header declares."""
    return set(DECLARED.findall(header_text))


def referenced_entry_points(sources: dict) -> set[str]:
    """Every `cyber_*` name the binding's sources mention.

    Callers strip comments first: a symbol named only in a doc comment is not
    bound, and counting it would let a binding document its way to green.
    """
    found: set[str] = set()
    for text in sources.values():
        found.update(REFERENCED.findall(text))
    return found


def coverage(
    sources: dict, header_text: str, pending: dict
) -> tuple[list[str], list[str], list[str]]:
    """Three ways the registration list can be wrong.

    * ``unbound`` -- declared, bound by nothing, and not registered. The gap
      this whole module exists to catch.
    * ``stale`` -- registered but no longer declared, so the list has rotted
      into naming something that stopped existing.
    * ``redundant`` -- registered AND bound. Harmless to the build and corrosive
      to the list: a reader trusts it to say what is missing, and an entry that
      was quietly implemented makes it a document that has to be re-checked
      instead of read.
    """
    declared = declared_entry_points(header_text)
    referenced = referenced_entry_points(sources)
    unbound = sorted(declared - referenced - set(pending))
    stale = sorted(set(pending) - declared)
    redundant = sorted((set(pending) & referenced) & declared)
    return unbound, stale, redundant


def report(
    binding: str,
    unbound: list[str],
    stale: list[str],
    redundant: list[str],
    pending: dict,
) -> list[str]:
    """Human-readable FAIL lines; empty when the binding is covered."""
    lines = []
    for name in unbound:
        lines.append(
            f"FAIL: {name} is declared in cyber_capi.h, bound by no {binding} source, "
            f"and not listed in PENDING_REGISTRATIONS"
        )
    for name in stale:
        lines.append(
            f"FAIL: PENDING_REGISTRATIONS lists {name}, which cyber_capi.h no longer declares"
        )
    for name in redundant:
        lines.append(
            f"FAIL: PENDING_REGISTRATIONS lists {name} as pending, but {binding} binds it "
            f"-- delete the line"
        )
    return lines


def summary(binding: str, header_text: str, pending: dict) -> str:
    total = len(declared_entry_points(header_text))
    return (
        f"{binding}/C ABI coverage OK: {total - len(pending)} of {total} entry point(s) "
        f"bound, {len(pending)} deliberately pending"
    )
