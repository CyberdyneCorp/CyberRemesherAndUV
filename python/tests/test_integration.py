#!/usr/bin/env python3
"""Integration test: Python client against the live C++ bridge server.

Usage: test_integration.py <path-to-cyberbridge-server>

Launches the server harness, reads its ephemeral port, then drives the full
command set through the reference Python client. Exits non-zero on any failure.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # make `cyberbridge` importable

from cyberbridge import BridgeError, Client, load_obj, save_obj  # noqa: E402


def start_server(binary):
    proc = subprocess.Popen(
        [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
    )
    line = proc.stdout.readline().strip()
    if not line.startswith("LISTENING"):
        proc.kill()
        raise RuntimeError(f"server did not announce a port, got: {line!r}")
    return proc, int(line.split()[1])


def stop_server(proc):
    try:
        if proc.stdin:
            proc.stdin.write("quit\n")
            proc.stdin.flush()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def run(binary):
    proc, port = start_server(binary)
    try:
        # Wrong protocol version must be refused with a clear message.
        try:
            bad = Client()
            bad._sock = __import__("socket").create_connection(("127.0.0.1", port), timeout=5)
            reply = bad._exchange({"type": "hello", "protocol": 999})
            check(reply.get("type") == "reject", "incompatible client should be rejected")
            bad.close()
        finally:
            pass

        with Client().connect(port) as app:
            check(app.ping(), "ping failed")

            # Scripted OBJ push (spec scenario) + pull round-trip.
            with tempfile.TemporaryDirectory() as tmp:
                src = os.path.join(tmp, "quad.obj")
                with open(src, "w", encoding="utf-8") as handle:
                    handle.write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n")
                stats = app.push_target(load_obj(src))
                check(stats.get("vertices") == 4, "target vertex count")
                check(stats.get("faces") == 2, "target face count")

                present, mesh = app.pull_target()
                check(present, "target should be present after push")
                out = os.path.join(tmp, "out.obj")
                save_obj(out, mesh)
                reloaded = load_obj(out)
                check(reloaded["faces"] == [[0, 1, 2], [0, 2, 3]], "faces round-trip via OBJ")

            # EditMesh with UVs, change marker.
            changed, rev0 = app.query_changed(0)
            check(not changed, "no change before any push")
            edit = {
                "positions": [0, 0, 0, 1, 0, 0, 1, 1, 0],
                "faces": [[0, 1, 2]],
                "uvs": [0, 0, 1, 0, 1, 1],
            }
            app.push_editmesh(edit)
            pulled = app.pull_editmesh()
            check(pulled["uvs"] == [0, 0, 1, 0, 1, 1], "editmesh UVs round-trip")
            changed, rev1 = app.query_changed(rev0)
            check(changed and rev1 != rev0, "change marker advanced")

            # Remote action press/poll.
            app.add_action("bake", "Bake")
            check(app.poll_presses() == [], "no presses yet")
            # Simulate a user tap by sending press_action directly.
            app._ok({"type": "press_action", "id": "bake"})
            check(app.poll_presses() == ["bake"], "press event delivered")

            # Symmetry + camera.
            app.set_symmetry("y", True)
            axis, enabled = app.query_symmetry()
            check(axis == "y" and enabled, "symmetry round-trip")
            app.set_camera({"position": [2, 0, 0], "target": [0, 0, 0], "up": [0, 1, 0], "fov": 50})
            cam = app.get_camera()
            check(abs(cam["position"][0] - 2) < 1e-5, "camera round-trip")

            app.message("integration ok")
        print("bridge integration: OK")
    finally:
        stop_server(proc)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: test_integration.py <cyberbridge-server>", file=sys.stderr)
        raise SystemExit(2)
    try:
        run(sys.argv[1])
    except (AssertionError, BridgeError, RuntimeError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
    raise SystemExit(0)
