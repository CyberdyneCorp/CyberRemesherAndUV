#!/usr/bin/env python3
"""Sculpt handoff bridge: a synthetic producer, then one-command sculpt-to-asset.

The product story is `sculpt -> handoff -> remesh -> UV -> bake`. The engine's
half is the *receiving* one: it reads a versioned sculpt handoff
(docs/sculpt-handoff-format.md) and never links, or even knows about, whichever
engine produced it.

There is no sculpting engine in this repository, so this script plays the
producer: it emits a handoff PLY for a lumpy blob and then drives the CLI
against it, both from a file and over a pipe. Any producer that emits the same
format is equally a first-class citizen.

    python examples/18_sculpt_handoff.py                     # file + stdin demo
    python examples/18_sculpt_handoff.py --emit blob.ply     # just write a handoff
    python examples/18_sculpt_handoff.py --emit -            # handoff to stdout

Deliberately dependency-free (no numpy, no examples/common.py) so it runs under
ctest on a bare interpreter.
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

HANDOFF_MAJOR = 1
HANDOFF_MINOR = 0


def blob(rings=20, segments=40, radius=1.0):
    """A lumpy sphere: positions, normals, colors, material mix, triangles."""
    verts = []
    for j in range(1, rings):  # skip the poles: no degenerate fan triangles
        theta = math.pi * j / rings
        for i in range(segments):
            phi = 2.0 * math.pi * i / segments
            nx = math.sin(theta) * math.cos(phi)
            ny = math.sin(theta) * math.sin(phi)
            nz = math.cos(theta)
            bump = 1.0 + 0.12 * math.sin(5.0 * phi) * math.sin(4.0 * theta)
            r = radius * bump
            color = (0.5 + 0.5 * nx, 0.5 + 0.5 * ny, 0.5 + 0.5 * nz)
            mix = 0.5 + 0.5 * math.sin(3.0 * theta)
            verts.append(((nx * r, ny * r, nz * r), (nx, ny, nz), color, mix))

    # Poles, added last so the ring indexing above stays simple.
    north = len(verts)
    verts.append(((0.0, 0.0, radius), (0.0, 0.0, 1.0), (1.0, 1.0, 1.0), 0.5))
    south = len(verts)
    verts.append(((0.0, 0.0, -radius), (0.0, 0.0, -1.0), (1.0, 1.0, 1.0), 0.5))

    tris = []
    for j in range(rings - 2):
        for i in range(segments):
            nxt = (i + 1) % segments
            a = j * segments + i
            b = j * segments + nxt
            c = (j + 1) * segments + nxt
            d = (j + 1) * segments + i
            tris.append((a, c, b))
            tris.append((a, d, c))
    for i in range(segments):
        nxt = (i + 1) % segments
        tris.append((north, i, nxt))
        last = (rings - 2) * segments
        tris.append((south, last + nxt, last + i))
    return verts, tris


def write_handoff(stream, verts, tris, major=HANDOFF_MAJOR, minor=HANDOFF_MINOR,
                  producer="examples/18_sculpt_handoff.py"):
    """Emits the ASCII PLY profile from docs/sculpt-handoff-format.md."""
    out = ["ply", "format ascii 1.0",
           f"comment cyber_sculpt_handoff {major} {minor}",
           f"comment cyber_handoff_producer {producer}",
           f"element vertex {len(verts)}",
           "property float x", "property float y", "property float z",
           "property float nx", "property float ny", "property float nz",
           "property uchar red", "property uchar green", "property uchar blue",
           "property float material_mix",
           f"element face {len(tris)}",
           "property list uchar int vertex_indices",
           "end_header"]
    for (p, n, c, mix) in verts:
        rgb = tuple(max(0, min(255, int(round(v * 255)))) for v in c)
        out.append(f"{p[0]:.6f} {p[1]:.6f} {p[2]:.6f} {n[0]:.6f} {n[1]:.6f} {n[2]:.6f} "
                   f"{rgb[0]} {rgb[1]} {rgb[2]} {mix:.4f}")
    for t in tris:
        out.append(f"3 {t[0]} {t[1]} {t[2]}")
    stream.write("\n".join(out) + "\n")


def find_cli():
    if os.environ.get("CYBER_CLI"):
        return os.environ["CYBER_CLI"]
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in ("build/apps/cli/cyberremesh", "build/cyberremesh"):
        path = os.path.join(os.path.dirname(here), candidate)
        if os.path.exists(path):
            return path
    return "cyberremesh"


def show_report(path):
    with open(path) as f:
        report = json.load(f)
    handoff = report.get("handoff", {})
    version = handoff.get("version", {})
    print(f"  source           {handoff.get('source')}")
    print(f"  handoff version  {version.get('major')}.{version.get('minor')}")
    print(f"  producer         {handoff.get('producer')}")
    print(f"  vertex colors    {handoff.get('hasVertexColors')}")
    print(f"  material mix     {handoff.get('hasMaterialMix')}")
    print(f"  quads out        {report.get('statistics', {}).get('quads')}")
    for out in report.get("outputs", []):
        print(f"  wrote            {out.get('kind'):<12} {out.get('path')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit", metavar="PATH",
                        help="write a handoff and exit ('-' for stdout)")
    args = parser.parse_args()

    verts, tris = blob()
    if args.emit:
        if args.emit == "-":
            write_handoff(sys.stdout, verts, tris)
        else:
            with open(args.emit, "w") as f:
                write_handoff(f, verts, tris)
            print(f"wrote {args.emit} ({len(verts)} vertices, {len(tris)} triangles)",
                  file=sys.stderr)
        return 0

    cli = find_cli()
    workdir = tempfile.mkdtemp(prefix="cyber_handoff_demo_")
    handoff_path = os.path.join(workdir, "sculpt.ply")
    with open(handoff_path, "w") as f:
        write_handoff(f, verts, tris)
    print(f"producer wrote {handoff_path} "
          f"({len(verts)} vertices, {len(tris)} triangles)\n")

    # 1. From a file, in one command: handoff -> remesh -> unwrap -> bake.
    report = os.path.join(workdir, "file.json")
    cmd = [cli, "--target", handoff_path, "--output", os.path.join(workdir, "low.obj"),
           "--target-quads", "300", "--preset", "blender",
           "--bake", "normal,ao,curvature", "--texture-size", "256",
           "--report", report, "--quiet"]
    print("$ " + " ".join(cmd))
    if subprocess.run(cmd).returncode != 0:
        print("handoff run failed", file=sys.stderr)
        return 1
    show_report(report)

    # 2. The same thing over a pipe — the `producer | cyberremesh` shape.
    print("\n$ python examples/18_sculpt_handoff.py --emit - | cyberremesh --target - ...")
    pipe_report = os.path.join(workdir, "stdin.json")
    cmd = [cli, "--target", "-", "--output", os.path.join(workdir, "low_pipe.obj"),
           "--target-quads", "300", "--report", pipe_report, "--quiet"]
    with open(handoff_path, "rb") as f:
        if subprocess.run(cmd, stdin=f).returncode != 0:
            print("stdin handoff run failed", file=sys.stderr)
            return 1
    show_report(pipe_report)

    # 3. A version this engine does not support is rejected loudly, naming both.
    future = os.path.join(workdir, "future.ply")
    with open(future, "w") as f:
        write_handoff(f, verts, tris, major=HANDOFF_MAJOR + 1, minor=0)
    print(f"\n$ cyberremesh --target {future} ...")
    done = subprocess.run([cli, "--target", future, "--output",
                           os.path.join(workdir, "never.obj"), "--quiet"],
                          capture_output=True, text=True)
    print(f"  exit {done.returncode}: {done.stderr.strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
