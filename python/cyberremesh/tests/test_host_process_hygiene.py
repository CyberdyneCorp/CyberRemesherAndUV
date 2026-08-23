#!/usr/bin/env python3
"""Host-process hygiene of the shipped shared library.

The engine is meant to be embedded: a DCC plugin, a farm worker, a service.
Such a host owns its process, and a single `cyber_remesh` used to take large
parts of it over. The vendored Geogram field solver behind the DEFAULT
quad-cover path called `GEO::initialize()` with its handler-installing default,
which

  * replaced the host's SIGSEGV/SIGILL/SIGBUS/SIGFPE handlers (so a crash
    reporter installed at host startup silently stopped producing minidumps),
  * reset SIGINT to the default disposition,
  * replaced `std::terminate` and `std::new_handler`, and
  * rewrote LC_NUMERIC in the process environment,

and the solver traced hundreds of lines of its own progress to the host's
stderr with no way to turn it off — not even the CLI's `--quiet`.

Every one of those is checked here in a FRESH process, which is what makes this
the real regression guard: the handler takeover happened on the FIRST remesh of
a process, so only a test that owns its process from the start can see it.
The per-call console half is additionally guarded in C++ (tests/quadrangulate/
test_quadcover_extractor.cpp).

Runnable as a plain script; exits 77 (CTest SKIP) if the library is absent.
"""

import ctypes
import math
import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh  # noqa: E402

FAILURES: "list" = []

# Read the disposition back with sigaction(sig, NULL, buf) into an oversized
# opaque buffer, and compare only the leading handler pointer: it is the first
# member of `struct sigaction` on every POSIX platform, and it is what decides
# whose code runs on a fault. (The rest of the record is not comparable — glibc
# leaves part of the returned sa_mask padding untouched, so it carries whatever
# was in the buffer's memory.)
_SIGACTION_BUF = 512
_HANDLER_BYTES = 8

_WATCHED_SIGNALS = ("SIGSEGV", "SIGILL", "SIGBUS", "SIGFPE", "SIGINT")


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def write_sphere(path: str, rings: int = 16, segments: int = 24) -> None:
    """Closed UV sphere — a smooth mesh, so it routes to the vendored solver."""
    lines = ["v 0 0 1"]
    for r in range(1, rings):
        phi = math.pi * r / rings
        for s in range(segments):
            th = 2.0 * math.pi * s / segments
            lines.append("v {0:.6f} {1:.6f} {2:.6f}".format(
                math.sin(phi) * math.cos(th), math.sin(phi) * math.sin(th), math.cos(phi)))
    lines.append("v 0 0 -1")
    south = 2 + (rings - 1) * segments

    def ring(r: int, s: int) -> int:
        return 2 + (r - 1) * segments + (s % segments)

    for s in range(segments):
        lines.append(f"f 1 {ring(1, s)} {ring(1, s + 1)}")
    for r in range(1, rings - 1):
        for s in range(segments):
            lines.append(f"f {ring(r, s)} {ring(r + 1, s)} {ring(r + 1, s + 1)}")
            lines.append(f"f {ring(r, s)} {ring(r + 1, s + 1)} {ring(r, s + 1)}")
    for s in range(segments):
        lines.append(f"f {south} {ring(rings - 1, s + 1)} {ring(rings - 1, s)}")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def disposition(libc, sig: int) -> bytes:
    """The handler currently installed for `sig`, as opaque bytes."""
    buf = ctypes.create_string_buffer(_SIGACTION_BUF)
    if libc.sigaction(ctypes.c_int(sig), None, buf) != 0:
        return b""
    return buf.raw[:_HANDLER_BYTES]


def remesh_capturing_stderr(src: str, out_path: str) -> bytes:
    """One default remesh with fd 2 pointed at a file; returns what it got."""
    from cyberremesh import Mesh, RemeshParams, remesh

    saved = os.dup(2)
    sink = os.open(out_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    try:
        os.dup2(sink, 2)
        with Mesh.load_obj(src) as mesh:
            with remesh(mesh, RemeshParams(target_quad_count=400)) as result:
                assert result.face_count > 0
    finally:
        os.dup2(saved, 2)
        os.close(saved)
        os.close(sink)
    with open(out_path, "rb") as fh:
        return fh.read()


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE

    posix = os.name == "posix"
    libc = ctypes.CDLL(None, use_errno=True) if posix else None
    signals: "dict" = {}
    lc_before = None
    if posix:
        import signal as signal_mod

        libc.getenv.restype = ctypes.c_char_p

        # A sentinel the library must not overwrite. It is only ever consulted
        # by a later setlocale(), so setting it here is harmless.
        os.environ["LC_NUMERIC"] = "en_US.UTF-8"
        lc_before = libc.getenv(b"LC_NUMERIC")
        for name in _WATCHED_SIGNALS:
            sig = getattr(signal_mod, name, None)
            if sig is not None:
                signals[name] = (int(sig), disposition(libc, int(sig)))

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "sphere.obj")
        write_sphere(src)
        captured = remesh_capturing_stderr(src, os.path.join(tmp, "stderr.txt"))

    # A library must not write to its host's console uninvited.
    check("a default remesh writes nothing to the host's stderr",
          captured == b"", f"got {len(captured)} bytes: {captured[:160]!r}")

    if not posix:
        print("note: signal/locale checks are POSIX-only and were skipped")
    else:
        for name, (sig, before) in signals.items():
            check(f"{name} disposition survives a remesh",
                  disposition(libc, sig) == before)
        check("LC_NUMERIC survives a remesh",
              libc.getenv(b"LC_NUMERIC") == lc_before,
              f"{lc_before!r} -> {libc.getenv(b'LC_NUMERIC')!r}")

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("\nthe engine leaves the host process as it found it")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except cyberremesh.CyberError as exc:
        print(f"FAIL: engine error {exc}")
        sys.exit(1)
    except Exception as exc:  # library missing -> CTest SKIP
        if "LibraryNotFound" in type(exc).__name__ or "Failed to load" in str(exc):
            print(f"SKIP: {exc}")
            sys.exit(77)
        raise
