"""Reference client for the CyberRemesher local bridge protocol.

Wire format: a 4-byte big-endian length prefix followed by a UTF-8 JSON
payload. Every connection opens with a version handshake. This mirrors the C++
client in ``src/net`` exactly, so either can talk to the same server.
"""

from __future__ import annotations

import json
import socket
import struct
from typing import Any, Dict, List, Optional, Tuple

PROTOCOL_VERSION = 1


class BridgeError(Exception):
    """Raised on a handshake rejection, transport failure, or server error."""


class Client:
    """Synchronous request/response client for the bridge.

    Use as a context manager::

        with Client().connect(port) as c:
            c.push_target(load_obj("model.obj"))
    """

    def __init__(self) -> None:
        self._sock: Optional[socket.socket] = None

    # -- connection ---------------------------------------------------------
    def connect(self, port: int, host: str = "127.0.0.1", timeout: float = 5.0) -> "Client":
        sock = socket.create_connection((host, port), timeout=timeout)
        self._sock = sock
        reply = self._exchange(
            {"type": "hello", "protocol": PROTOCOL_VERSION, "client": "cyber-python"}
        )
        if reply.get("type") != "welcome":
            self.close()
            raise BridgeError(f"handshake rejected: {reply.get('reason', 'unknown')}")
        return self

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *_exc: Any) -> None:
        self.close()

    # -- transport ----------------------------------------------------------
    def _read_exact(self, n: int) -> bytes:
        assert self._sock is not None
        chunks: List[bytes] = []
        remaining = n
        while remaining > 0:
            chunk = self._sock.recv(remaining)
            if not chunk:
                raise BridgeError("connection closed by server")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _exchange(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        if self._sock is None:
            raise BridgeError("not connected")
        data = json.dumps(payload).encode("utf-8")
        self._sock.sendall(struct.pack(">I", len(data)) + data)
        (length,) = struct.unpack(">I", self._read_exact(4))
        return json.loads(self._read_exact(length).decode("utf-8"))

    def _ok(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        reply = self._exchange(payload)
        if reply.get("type") == "error":
            raise BridgeError(reply.get("message", "server error"))
        return reply

    # -- commands -----------------------------------------------------------
    def ping(self) -> bool:
        return self._exchange({"type": "ping"}).get("type") == "pong"

    def push_target(self, mesh: Dict[str, Any]) -> Dict[str, Any]:
        return self._ok({"type": "push_target", "mesh": mesh})

    def pull_target(self) -> Tuple[bool, Dict[str, Any]]:
        reply = self._ok({"type": "pull_target"})
        return bool(reply.get("present", False)), reply.get("mesh", {})

    def push_editmesh(self, mesh: Dict[str, Any]) -> int:
        return int(self._ok({"type": "push_editmesh", "mesh": mesh}).get("revision", 0))

    def pull_editmesh(self) -> Dict[str, Any]:
        return self._ok({"type": "pull_editmesh"}).get("mesh", {})

    def clear_scene(self) -> None:
        self._ok({"type": "clear_scene"})

    def close_document(self) -> None:
        self._ok({"type": "close_document"})

    def message(self, text: str) -> None:
        self._ok({"type": "message", "text": text})

    def add_action(self, action_id: str, label: str = "") -> None:
        self._ok({"type": "add_action", "id": action_id, "label": label})

    def remove_action(self, action_id: str) -> None:
        self._ok({"type": "remove_action", "id": action_id})

    def poll_presses(self) -> List[str]:
        return list(self._ok({"type": "poll_presses"}).get("ids", []))

    def set_symmetry(self, axis: str, enabled: bool) -> None:
        self._ok({"type": "set_symmetry", "axis": axis, "enabled": enabled})

    def query_symmetry(self) -> Tuple[str, bool]:
        reply = self._ok({"type": "query_symmetry"})
        return str(reply.get("axis", "none")), bool(reply.get("enabled", False))

    def query_changed(self, marker: int) -> Tuple[bool, int]:
        reply = self._ok({"type": "query_changed", "marker": marker})
        return bool(reply.get("changed", False)), int(reply.get("revision", 0))

    def set_camera(self, pose: Dict[str, Any]) -> None:
        self._ok({"type": "set_camera", "pose": pose})

    def get_camera(self) -> Dict[str, Any]:
        return self._ok({"type": "get_camera"}).get("pose", {})


# -- OBJ helpers (positions + faces; enough for scripted push/pull) ---------
def load_obj(path: str) -> Dict[str, Any]:
    """Read an OBJ into a wire mesh (positions flat, faces 0-indexed)."""
    positions: List[float] = []
    faces: List[List[int]] = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "v" and len(parts) >= 4:
                positions.extend(float(x) for x in parts[1:4])
            elif parts[0] == "f":
                # OBJ faces are 1-indexed and may carry v/vt/vn tuples.
                faces.append([int(p.split("/")[0]) - 1 for p in parts[1:]])
    return {"positions": positions, "faces": faces}


def save_obj(path: str, mesh: Dict[str, Any]) -> None:
    """Write a wire mesh back out as OBJ."""
    positions = mesh.get("positions", [])
    faces = mesh.get("faces", [])
    with open(path, "w", encoding="utf-8") as handle:
        for i in range(0, len(positions), 3):
            handle.write(f"v {positions[i]} {positions[i + 1]} {positions[i + 2]}\n")
        for face in faces:
            handle.write("f " + " ".join(str(idx + 1) for idx in face) + "\n")
