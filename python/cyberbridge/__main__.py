"""Command-line interface for the CyberRemesher bridge client.

Examples::

    python -m cyberbridge --port 5140 ping
    python -m cyberbridge --port 5140 push-target model.obj
    python -m cyberbridge --port 5140 pull-editmesh out.obj
    python -m cyberbridge --port 5140 message "remesh done"
"""

from __future__ import annotations

import argparse
import sys

from .client import BridgeError, Client, load_obj, save_obj


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="cyberbridge", description="CyberRemesher bridge client")
    parser.add_argument("--port", type=int, required=True, help="bridge port on 127.0.0.1")
    parser.add_argument("--host", default="127.0.0.1")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ping", help="check the connection")
    p_push = sub.add_parser("push-target", help="load an OBJ as the Target")
    p_push.add_argument("obj")
    p_pull = sub.add_parser("pull-editmesh", help="save the current EditMesh to OBJ")
    p_pull.add_argument("obj")
    p_msg = sub.add_parser("message", help="show a message in the app")
    p_msg.add_argument("text")

    args = parser.parse_args(argv)

    try:
        with Client().connect(args.port, host=args.host) as client:
            if args.command == "ping":
                print("ok" if client.ping() else "no response")
            elif args.command == "push-target":
                stats = client.push_target(load_obj(args.obj))
                print(f"pushed target: {stats.get('vertices', 0)} verts, {stats.get('faces', 0)} faces")
            elif args.command == "pull-editmesh":
                save_obj(args.obj, client.pull_editmesh())
                print(f"wrote {args.obj}")
            elif args.command == "message":
                client.message(args.text)
                print("sent")
    except (BridgeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
