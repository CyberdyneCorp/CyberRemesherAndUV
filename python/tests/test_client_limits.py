#!/usr/bin/env python3
"""Regression test: the Python client refuses an oversized or malformed reply.

No C++ binary is needed — a stub server on loopback stands in for the bridge,
which is the only way to drive the client with replies a conforming server
never sends. Every read is bounded so a regression fails instead of hanging or
exhausting memory.
"""

import json
import os
import re
import socket
import struct
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # make `cyberbridge` importable

from cyberbridge import MAX_MESSAGE_BYTES, BridgeError, Client  # noqa: E402

WELCOME = {"type": "welcome", "protocol": 1, "app": "stub"}


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


class StubServer:
    """Bridge stand-in: answers the handshake, then replies with canned frames.

    ``raw_replies`` are byte strings written verbatim (length prefix included),
    so a reply can declare a length it never intends to deliver.
    """

    def __init__(self, raw_replies):
        self._raw_replies = list(raw_replies)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(1)
        self.port = self._sock.getsockname()[1]
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        try:
            conn, _ = self._sock.accept()
        except OSError:
            return
        with conn:
            conn.settimeout(5.0)
            replies = [frame(WELCOME)] + self._raw_replies
            for reply in replies:
                if not read_frame(conn):
                    return
                conn.sendall(reply)
            # Hold the connection open so a client that keeps reading hits its
            # own timeout rather than a clean EOF.
            try:
                conn.recv(1)
            except OSError:
                pass

    def close(self):
        self._sock.close()
        self._thread.join(timeout=5.0)


def frame(payload):
    data = json.dumps(payload).encode("utf-8")
    return struct.pack(">I", len(data)) + data


def read_frame(conn):
    header = conn.recv(4)
    if len(header) < 4:
        return False
    (length,) = struct.unpack(">I", header)
    remaining = length
    while remaining > 0:
        chunk = conn.recv(remaining)
        if not chunk:
            return False
        remaining -= len(chunk)
    return True


def expect_bridge_error(fn, what):
    try:
        fn()
    except BridgeError:
        return
    except Exception as exc:  # noqa: BLE001 - any other type is the regression
        raise AssertionError(f"{what}: expected BridgeError, got {type(exc).__name__}: {exc}")
    raise AssertionError(f"{what}: expected BridgeError, call returned normally")


def test_oversized_length_is_refused():
    # A length prefix of 0xFFFFFFFF followed by a token payload. The client used
    # to buffer whatever arrived, up to the 4 GiB the u32 allows; it must refuse
    # on the declared length alone, before reading any of it.
    oversized = struct.pack(">I", 0xFFFFFFFF) + b"x" * 4096
    stub = StubServer([oversized])
    try:
        client = Client().connect(stub.port, timeout=5.0)
        expect_bridge_error(client.ping, "oversized reply")
        client.close()
    finally:
        stub.close()


def test_length_just_above_the_limit_is_refused():
    stub = StubServer([struct.pack(">I", MAX_MESSAGE_BYTES + 1) + b"x" * 4096])
    try:
        client = Client().connect(stub.port, timeout=5.0)
        expect_bridge_error(client.ping, "reply one byte over the limit")
        client.close()
    finally:
        stub.close()


def test_non_object_reply_is_a_bridge_error():
    stub = StubServer([frame([1, 2, 3])])
    try:
        client = Client().connect(stub.port, timeout=5.0)
        expect_bridge_error(client.ping, "top-level array reply")
        client.close()
    finally:
        stub.close()


def test_conforming_replies_still_work():
    stub = StubServer([frame({"type": "pong"}), frame({"type": "ok", "revision": 3})])
    try:
        with Client().connect(stub.port, timeout=5.0) as client:
            check(client.ping(), "ping should succeed against a conforming reply")
            check(client.push_editmesh({}) == 3, "revision should round-trip")
    finally:
        stub.close()


def test_limit_mirrors_the_cpp_constant():
    header = os.path.join(
        os.path.dirname(HERE), "..", "src", "net", "include", "cyber", "net", "protocol.hpp"
    )
    if not os.path.exists(header):
        return  # source tree not available (installed package)
    with open(header, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"kMaxMessageBytes\s*=\s*([0-9]+)u?\s*\*\s*([0-9]+)u?\s*\*\s*([0-9]+)u?", text)
    check(match is not None, "kMaxMessageBytes not found in protocol.hpp")
    expected = int(match.group(1)) * int(match.group(2)) * int(match.group(3))
    check(
        MAX_MESSAGE_BYTES == expected,
        f"MAX_MESSAGE_BYTES {MAX_MESSAGE_BYTES} != kMaxMessageBytes {expected}",
    )


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  {name}: OK")
    print("bridge client limits: OK")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
    raise SystemExit(0)
