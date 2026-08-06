#!/usr/bin/env python3
"""CLI integration tests (cli-headless spec, task 6.3): every exit code and
the clamp-warning path, run against the real binary."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
FAILURES: list[str] = []


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(BINARY), *args], capture_output=True, text=True, timeout=300)


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def write_sphere_obj(path: Path) -> None:
    import math

    rings, segments = 10, 14
    verts = [(0.0, 0.0, 1.0)]
    for r in range(1, rings):
        phi = math.pi * r / rings
        for s in range(segments):
            theta = 2 * math.pi * s / segments
            verts.append(
                (math.sin(phi) * math.cos(theta), math.sin(phi) * math.sin(theta), math.cos(phi))
            )
    verts.append((0.0, 0.0, -1.0))
    south = len(verts)

    def ring(r: int, s: int) -> int:
        return 2 + (r - 1) * segments + (s % segments)  # 1-based OBJ indices

    faces = []
    for s in range(segments):
        faces.append((1, ring(1, s), ring(1, s + 1)))
    for r in range(1, rings - 1):
        for s in range(segments):
            faces.append((ring(r, s), ring(r + 1, s), ring(r + 1, s + 1)))
            faces.append((ring(r, s), ring(r + 1, s + 1), ring(r, s + 1)))
    for s in range(segments):
        faces.append((south, ring(rings - 1, s + 1), ring(rings - 1, s)))

    with path.open("w") as f:
        for v in verts:
            f.write(f"v {v[0]} {v[1]} {v[2]}\n")
        for face in faces:
            f.write(f"f {face[0]} {face[1]} {face[2]}\n")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="cyber_cli_"))
    sphere = tmp / "sphere.obj"
    write_sphere_obj(sphere)
    out = tmp / "out.obj"
    report = tmp / "report.json"

    # --version prints a real version (exit 0).
    r = run("--version")
    check("version exit 0", r.returncode == 0)
    check("version non-empty", "cyberremesh 0." in r.stdout, r.stdout)

    # Success path with report (exit 0).
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--report", str(report), "--quiet")
    check("success exit 0", r.returncode == 0, r.stderr)
    check("output written", out.exists())
    check("report written", report.exists())
    if report.exists():
        data = json.loads(report.read_text())
        check("report status", data["status"] == "success", str(data))
        check("report quads > 0", data["statistics"]["quads"] > 0)
        check("report islands", data["statistics"]["islands"] == 1)

    # Argument errors (exit 2).
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "abc")
    check("non-numeric flag exit 2", r.returncode == 2, str(r.returncode))
    check("non-numeric names value", "abc" in r.stderr, r.stderr)
    r = run("--input", str(sphere))
    check("missing output exit 2", r.returncode == 2)
    r = run("--bogus-flag")
    check("unknown flag exit 2", r.returncode == 2)

    # Load failure (exit 3) — AutoRemesher exited 0 here.
    r = run("--input", str(tmp / "missing.obj"), "--output", str(out))
    check("missing input exit 3", r.returncode == 3, str(r.returncode))
    check("missing input message", "missing.obj" in r.stderr, r.stderr)

    # Write failure (exit 6). Small target keeps the failing runs fast.
    r = run("--input", str(sphere), "--output", "/nonexistent_dir_xyz/out.obj",
            "--target-quads", "300", "--quiet")
    check("unwritable output exit 6", r.returncode == 6, str(r.returncode))
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--report", "/nonexistent_dir_xyz/report.json", "--quiet")
    check("unwritable report exit 6", r.returncode == 6, str(r.returncode))

    # Clamp warning printed, run proceeds (exit 0).
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--edge-scale", "10.0")
    check("clamped run exit 0", r.returncode == 0, r.stderr)
    check("clamp warning printed", "clamped" in r.stderr, r.stderr)

    # Pure quads through the CLI.
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--pure-quads", "--report", str(report), "--quiet")
    check("pure quads exit 0", r.returncode == 0, r.stderr)
    if report.exists():
        data = json.loads(report.read_text())
        check("pure quads: zero triangles", data["statistics"]["triangles"] == 0, str(data))

    # --- export presets ------------------------------------------------
    r = run("--list-presets")
    check("list-presets exit 0", r.returncode == 0)
    builtins = r.stdout.split()
    check("list-presets names four", builtins == ["blender", "unity", "unreal", "gltf-generic"],
          r.stdout)

    # An unknown preset is an argument error that names the alternatives, and
    # it fails BEFORE the remesh rather than after a full solve.
    r = run("--input", str(sphere), "--output", str(out), "--preset", "nosuch")
    check("unknown preset exit 2", r.returncode == 2, str(r.returncode))
    check("unknown preset lists built-ins", "blender" in r.stderr, r.stderr)

    preset_dir = tmp / "bundle"
    preset_dir.mkdir()
    preset_out = preset_dir / "hero.obj"
    # --texture-size keeps the test fast: the AO bake is ~96% of a preset run's
    # cost and scales with texel count, so the default 2048 would be minutes.
    r = run("--input", str(sphere), "--output", str(preset_out), "--target-quads", "300",
            "--preset", "blender", "--texture-size", "64", "--ao-samples", "4",
            "--report", str(report), "--quiet")
    check("preset run exit 0", r.returncode == 0, r.stderr)
    for name in ("hero.obj", "hero_normal.png", "hero_ao.png", "hero_curvature.png",
                 "hero_color.png"):
        check(f"preset emitted {name}", (preset_dir / name).exists())
    if report.exists():
        data = json.loads(report.read_text())
        check("report names the preset", data.get("preset", {}).get("name") == "blender",
              str(data.get("preset")))
        check("report records schema version", data["preset"]["schemaVersion"] == 1)
        check("report lists every output", len(data.get("outputs", [])) == 5,
              str(data.get("outputs")))
        kinds = [o["kind"] for o in data["outputs"]]
        check("report kinds", kinds == ["mesh", "normal", "ao", "curvature", "color"], str(kinds))
        colors = {o["kind"]: o.get("colorSpace") for o in data["outputs"] if "colorSpace" in o}
        check("color is sRGB, data maps linear",
              colors == {"normal": "linear", "ao": "linear", "curvature": "linear",
                         "color": "srgb"}, str(colors))

    # A user preset file behaves exactly like a built-in.
    user_preset = tmp / "mine.json"
    user_preset.write_text(json.dumps({
        "schemaVersion": 1, "name": "mine", "resolution": 64,
        "namingPattern": "{basename}.{map}.{ext}", "maps": ["curvature"],
    }))
    check("report time covers the bake", json.loads(report.read_text())["elapsedSeconds"] > 0.0)
    user_dir = tmp / "user"
    user_dir.mkdir()
    r = run("--input", str(sphere), "--output", str(user_dir / "u.obj"), "--target-quads", "300",
            "--preset", str(user_preset), "--quiet")
    check("user preset exit 0", r.returncode == 0, r.stderr)
    check("user preset naming honored", (user_dir / "u.curvature.png").exists())

    # An incompatible schema version fails loudly and produces nothing.
    bad_preset = tmp / "future.json"
    bad_preset.write_text(json.dumps({
        "schemaVersion": 99, "name": "future", "maps": ["normal"],
    }))
    bad_dir = tmp / "bad"
    bad_dir.mkdir()
    r = run("--input", str(sphere), "--output", str(bad_dir / "b.obj"), "--target-quads", "300",
            "--preset", str(bad_preset), "--quiet")
    check("future preset exit 2", r.returncode == 2, str(r.returncode))
    check("future preset names both versions", "99" in r.stderr, r.stderr)
    check("future preset wrote nothing", not any(bad_dir.iterdir()), str(list(bad_dir.iterdir())))

    print(f"\n{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
