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


def write_box_obj(path: Path, n: int = 6) -> None:
    """Closed unit box, n cells per side, triangulated and welded."""
    verts: list[tuple[float, float, float]] = []
    index: dict[tuple[int, int, int], int] = {}
    faces: list[tuple[int, int, int]] = []

    def vid(axis: int, side: int, u: int, v: int) -> int:
        key = [0, 0, 0]
        key[axis] = side
        key[(axis + 1) % 3] = u
        key[(axis + 2) % 3] = v
        k = (key[0], key[1], key[2])
        if k not in index:
            index[k] = len(verts) + 1  # 1-based OBJ indices
            verts.append((k[0] / n, k[1] / n, k[2] / n))
        return index[k]

    for axis in range(3):
        for side in (0, n):
            for i in range(n):
                for j in range(n):
                    a, b = vid(axis, side, i, j), vid(axis, side, i + 1, j)
                    c, d = vid(axis, side, i + 1, j + 1), vid(axis, side, i, j + 1)
                    faces.append((a, b, c))
                    faces.append((a, c, d))

    with path.open("w") as f:
        for v in verts:
            f.write(f"v {v[0]} {v[1]} {v[2]}\n")
        for face in faces:
            f.write(f"f {face[0]} {face[1]} {face[2]}\n")


def handoff_ply(major: int, minor: int) -> str:
    """The sculpt handoff PLY profile (docs/sculpt-handoff-format.md) for a
    small closed box. The version is a parameter so a test can emit one the
    engine has to refuse."""
    corners = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
               (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = [t for q in quads for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))]
    lines = ["ply", "format ascii 1.0",
             f"comment cyber_sculpt_handoff {major} {minor}",
             "comment cyber_handoff_producer cli-test",
             f"element vertex {len(corners)}",
             "property float x", "property float y", "property float z",
             "property float nx", "property float ny", "property float nz",
             "property uchar red", "property uchar green", "property uchar blue",
             "property float material_mix",
             f"element face {len(tris)}",
             "property list uchar int vertex_indices",
             "end_header"]
    for i, c in enumerate(corners):
        n = [c[k] - 0.5 for k in range(3)]
        lines.append(f"{c[0]} {c[1]} {c[2]} {n[0]} {n[1]} {n[2]} 255 0 0 {i / 8.0}")
    for t in tris:
        lines.append(f"3 {t[0]} {t[1]} {t[2]}")
    return "\n".join(lines) + "\n"


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

    # The clamp must be real, not cosmetic: an out-of-range --adaptivity used to
    # drive the quad-cover solve unclamped (a different mesh from --adaptivity 1)
    # while the warning said it had been clamped and the report echoed the raw
    # value. Both runs write to the SAME path so the exporter's `mtllib` line,
    # which names the output, cannot make identical meshes compare unequal.
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--adaptivity", "7", "--report", str(report))
    check("out-of-range adaptivity exit 0", r.returncode == 0, r.stderr)
    check("out-of-range adaptivity warns", "clamped" in r.stderr, r.stderr)
    clamped_mesh = out.read_bytes()
    data = json.loads(report.read_text())
    check("report records the post-clamp adaptivity", data["parameters"]["adaptivity"] == 1.0,
          str(data["parameters"]))
    check("report keeps the clamp warning",
          any("adaptivity" in w for w in data["warnings"]), str(data["warnings"]))
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--adaptivity", "1", "--quiet")
    check("out-of-range adaptivity reproduces the in-range mesh",
          r.returncode == 0 and out.read_bytes() == clamped_mesh, r.stderr)

    # Same for --sharp-edge, which reaches the solve on the same route.
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--sharp-edge", "5", "--report", str(report))
    check("out-of-range sharp-edge exit 0", r.returncode == 0, r.stderr)
    clamped_mesh = out.read_bytes()
    data = json.loads(report.read_text())
    check("report records the post-clamp sharp edge", data["parameters"]["sharpEdgeDegrees"] == 30.0,
          str(data["parameters"]))
    r = run("--input", str(sphere), "--output", str(out), "--target-quads", "300",
            "--sharp-edge", "30", "--quiet")
    check("out-of-range sharp-edge reproduces the in-range mesh",
          r.returncode == 0 and out.read_bytes() == clamped_mesh, r.stderr)

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

    # ---- guidance sidecar (--guides) ------------------------------------
    guides_dir = tmp / "guides"
    guides_dir.mkdir()

    good = guides_dir / "good.json"
    good.write_text(json.dumps({
        "version": 1,
        "guides": [{"points": [[0.0, 0.0, 1.0], [0.5, 0.0, 0.7], [0.9, 0.0, 0.2]],
                    "strength": 5.0,   # out of range: must clamp to 1.0 and be reported
                    "radius": 0.4}],
    }))
    guided_report = guides_dir / "guided.json"
    r = run("--input", str(sphere), "--output", str(guides_dir / "g.obj"), "--target-quads", "300",
            "--guides", str(good), "--report", str(guided_report), "--quiet")
    check("guides happy path exit 0", r.returncode == 0, r.stderr)
    data = json.loads(guided_report.read_text())
    check("report has a guidance block", "guidance" in data, str(list(data)))
    block = data.get("guidance", {})
    check("report records the POST-CLAMP strength",
          block.get("guides", [{}])[0].get("strength") == 1.0, json.dumps(block.get("guides")))
    check("report records the guide radius",
          abs(block.get("guides", [{}])[0].get("radius", 0.0) - 0.4) < 1e-5,
          json.dumps(block.get("guides")))
    check("report records the density clamp range",
          block.get("density", {}).get("clampRange") == [0.25, 4.0],
          json.dumps(block.get("density")))
    check("report names the clamp issue",
          any("strength" in i.get("parameter", "") for i in block.get("issues", [])),
          json.dumps(block.get("issues")))
    check("report has a per-island guidance row", len(block.get("islands", [])) >= 1,
          json.dumps(block.get("islands")))

    # A zero-radius guide could never be honored: exit 2, naming the file.
    zero = guides_dir / "zero.json"
    zero.write_text(json.dumps({
        "version": 1,
        "guides": [{"points": [[0, 0, 1], [1, 0, 0]], "strength": 1.0, "radius": 0.0}],
    }))
    r = run("--input", str(sphere), "--output", str(guides_dir / "z.obj"), "--target-quads", "300",
            "--guides", str(zero), "--quiet")
    check("zero-radius guide exit 2", r.returncode == 2, str(r.returncode))
    check("zero-radius guide names the file", str(zero) in r.stderr, r.stderr)

    # A malformed sidecar is exit 2, never a silent skip.
    broken = guides_dir / "broken.json"
    broken.write_text("{ this is not json")
    r = run("--input", str(sphere), "--output", str(guides_dir / "b.obj"), "--target-quads", "300",
            "--guides", str(broken), "--quiet")
    check("malformed sidecar exit 2", r.returncode == 2, str(r.returncode))
    check("malformed sidecar names the file", str(broken) in r.stderr, r.stderr)

    missing = guides_dir / "nope.json"
    r = run("--input", str(sphere), "--output", str(guides_dir / "m.obj"), "--target-quads", "300",
            "--guides", str(missing), "--quiet")
    check("missing sidecar exit 2", r.returncode == 2, str(r.returncode))

    # Valid JSON, wrong TYPES. Every one of these used to reach an unguarded
    # nlohmann accessor and abort the process (SIGABRT, exit 134); each must be
    # an exit-2 argument error naming the file and the offending field.
    type_cases = [
        ("version", {"version": "1"}, "version"),
        ("strength", {"version": 1,
                      "guides": [{"points": [[0, 0, 1], [1, 0, 0]], "strength": "strong",
                                  "radius": 0.4}]}, "strength"),
        ("radius", {"version": 1,
                    "guides": [{"points": [[0, 0, 1], [1, 0, 0]], "radius": "wide"}]}, "radius"),
        ("point", {"version": 1,
                   "guides": [{"points": [["a", "b", "c"], [1, 0, 0]], "radius": 0.4}]}, "point"),
        ("density.perVertex", {"version": 1, "density": {"perVertex": ["a"]}}, "perVertex"),
        ("density.perFace", {"version": 1, "density": {"perFace": [1.0, "x"]}}, "perFace"),
        ("density object", {"version": 1, "density": 3}, "density"),
        ("guides array", {"version": 1, "guides": 7}, "guides"),
    ]
    for label, payload, needle in type_cases:
        bad = guides_dir / f"type_{label.replace('.', '_').replace(' ', '_')}.json"
        bad.write_text(json.dumps(payload))
        r = run("--input", str(sphere), "--output", str(guides_dir / "t.obj"),
                "--target-quads", "300", "--guides", str(bad), "--quiet")
        check(f"sidecar type mismatch ({label}) exit 2", r.returncode == 2,
              f"exit {r.returncode}: {r.stderr}")
        check(f"sidecar type mismatch ({label}) names the file and the field",
              str(bad) in r.stderr and needle in r.stderr, r.stderr)

    # An EMPTY guide list must reproduce the no-guides output byte for byte.
    empty = guides_dir / "empty.json"
    empty.write_text(json.dumps({"version": 1, "guides": []}))
    # Same BASENAME in two directories: the OBJ writer emits an "mtllib
    # <basename>.mtl" line, so differing file names would differ trivially.
    plain_dir = guides_dir / "plain"
    empty_dir = guides_dir / "empty"
    plain_dir.mkdir()
    empty_dir.mkdir()
    plain_out = plain_dir / "out.obj"
    empty_out = empty_dir / "out.obj"
    r = run("--input", str(sphere), "--output", str(plain_out), "--target-quads", "300",
            "--quad-method", "field-aligned", "--quiet")
    check("no-guides run exit 0", r.returncode == 0, r.stderr)
    r = run("--input", str(sphere), "--output", str(empty_out), "--target-quads", "300",
            "--quad-method", "field-aligned", "--guides", str(empty), "--quiet")
    check("empty-guides run exit 0", r.returncode == 0, r.stderr)
    check("empty guide list is byte-identical to no --guides",
          plain_out.read_bytes() == empty_out.read_bytes())

    # A density of 1.0 everywhere must be a no-op on the SHIPPED DEFAULT method
    # (quad-cover), not just on field-aligned: supplying one used to switch the
    # seamless-UV route and change the mesh (remeshing-pipeline spec, "A density
    # of 1.0 everywhere SHALL reproduce today's uniform sizing byte-identically").
    # The input is a creased BOX, not the sphere: creases route quad-cover to the
    # native seamless solve, the bit-reproducible one. The vendored Geogram solve
    # a smooth mesh lands on is not (measured: 1 run in 30 differs), which would
    # make a byte comparison flaky for reasons that have nothing to do with
    # guidance.
    box = tmp / "box.obj"
    write_box_obj(box)
    vertex_count = sum(1 for line in box.read_text().splitlines() if line.startswith("v "))
    ones = guides_dir / "ones.json"
    ones.write_text(json.dumps({"version": 1, "density": {"perVertex": [1.0] * vertex_count}}))
    dens_dirs = {}
    for name in ("base", "repeat", "ones", "painted"):
        dens_dirs[name] = guides_dir / f"dens_{name}"
        dens_dirs[name].mkdir()
    base_out = dens_dirs["base"] / "out.obj"
    repeat_out = dens_dirs["repeat"] / "out.obj"
    ones_out = dens_dirs["ones"] / "out.obj"
    r = run("--input", str(box), "--output", str(base_out), "--target-quads", "300", "--quiet")
    check("default-method no-density run exit 0", r.returncode == 0, r.stderr)
    r = run("--input", str(box), "--output", str(repeat_out), "--target-quads", "300", "--quiet")
    # Guards the comparison below: it proves nothing if the run cannot repeat itself.
    check("default-method run repeats itself byte-for-byte",
          base_out.read_bytes() == repeat_out.read_bytes())
    ones_report = guides_dir / "ones_report.json"
    r = run("--input", str(box), "--output", str(ones_out), "--target-quads", "300",
            "--guides", str(ones), "--report", str(ones_report), "--quiet")
    check("density 1.0 run exit 0", r.returncode == 0, r.stderr)
    check("density 1.0 everywhere is byte-identical to no --guides (default method)",
          base_out.read_bytes() == ones_out.read_bytes())
    data = json.loads(ones_report.read_text())
    check("the ignored uniform density is reported, not silent",
          any("density" in w for w in data.get("warnings", [])), str(data.get("warnings")))

    # A non-uniform density is still a real request: it must change the mesh.
    painted = guides_dir / "painted.json"
    painted.write_text(json.dumps({
        "version": 1,
        "density": {"perVertex": [4.0 if i % 2 == 0 else 1.0 for i in range(vertex_count)]},
    }))
    painted_out = dens_dirs["painted"] / "out.obj"
    r = run("--input", str(box), "--output", str(painted_out), "--target-quads", "300",
            "--guides", str(painted), "--quiet")
    check("painted density run exit 0", r.returncode == 0, r.stderr)
    check("a painted density still changes the mesh",
          base_out.read_bytes() != painted_out.read_bytes())

    # ---- sculpt handoff Target (pipeline-bridge / cli-headless) ----------
    handoff_dir = tmp / "handoff"
    handoff_dir.mkdir()
    handoff = handoff_dir / "sculpt.ply"
    handoff.write_text(handoff_ply(1, 0))

    # One command: handoff -> remesh -> unwrap -> bake, with the report naming
    # the handoff version.
    handoff_report = handoff_dir / "handoff.json"
    handoff_out = handoff_dir / "low.obj"
    r = run("--target", str(handoff), "--output", str(handoff_out), "--target-quads", "120",
            "--preset", "blender", "--bake", "normal,ao,curvature",
            "--texture-size", "64", "--report", str(handoff_report), "--quiet")
    check("handoff run exit 0", r.returncode == 0, r.stderr)
    check("handoff run wrote the low-poly", handoff_out.exists())
    if handoff_report.exists():
        data = json.loads(handoff_report.read_text())
        block = data.get("handoff", {})
        check("report carries the handoff version",
              block.get("version", {}) == {"major": 1, "minor": 0}, str(block))
        check("report carries the handoff producer",
              block.get("producer") == "cli-test", str(block.get("producer")))
        check("report carries the handoff source",
              block.get("source") == str(handoff), str(block.get("source")))
        check("report records the handoff payloads",
              block.get("hasVertexColors") is True and block.get("hasMaterialMix") is True,
              str(block))
        kinds = {o.get("kind") for o in data.get("outputs", [])}
        check("--bake wrote exactly the requested maps",
              kinds == {"mesh", "normal", "ao", "curvature"}, str(kinds))
    else:
        check("handoff report written", False)

    # The same run with the handoff piped on stdin.
    stdin_report = handoff_dir / "stdin.json"
    stdin_out = handoff_dir / "low_stdin.obj"
    with handoff.open("rb") as f:
        r = subprocess.run([str(BINARY), "--target", "-", "--output", str(stdin_out),
                            "--target-quads", "120", "--report", str(stdin_report), "--quiet"],
                           stdin=f, capture_output=True, text=True, timeout=300)
    check("stdin handoff exit 0", r.returncode == 0, r.stderr)
    check("stdin handoff wrote the low-poly", stdin_out.exists())
    if stdin_report.exists():
        block = json.loads(stdin_report.read_text()).get("handoff", {})
        check("stdin report names <stdin> as the source",
              block.get("source") == "<stdin>", str(block.get("source")))
        check("stdin report carries the handoff version",
              block.get("version", {}) == {"major": 1, "minor": 0}, str(block))

    # --target and --input name the same slot: taking both is exit 2.
    r = run("--input", str(sphere), "--target", str(handoff),
            "--output", str(handoff_dir / "never.obj"), "--quiet")
    check("--target with --input is exit 2", r.returncode == 2, str(r.returncode))

    # An unsupported handoff version is exit 3, names both versions, writes
    # nothing.
    future = handoff_dir / "future.ply"
    future.write_text(handoff_ply(2, 0))
    future_out = handoff_dir / "future.obj"
    r = run("--target", str(future), "--output", str(future_out), "--quiet")
    check("future handoff is exit 3", r.returncode == 3, str(r.returncode))
    check("future handoff names both versions",
          "2.0" in r.stderr and "1.0" in r.stderr, r.stderr)
    check("future handoff wrote no output", not future_out.exists())

    # A PLY without the handoff comment is not a handoff (exit 3).
    plain = handoff_dir / "plain.ply"
    plain.write_text(handoff_ply(1, 0).replace("comment cyber_sculpt_handoff 1 0\n", ""))
    r = run("--target", str(plain), "--output", str(handoff_dir / "plain.obj"), "--quiet")
    check("a plain PLY is not a handoff", r.returncode == 3, str(r.returncode))

    # An unknown --bake map is an argument error naming the offender.
    r = run("--target", str(handoff), "--output", str(handoff_dir / "bad.obj"),
            "--bake", "normal,glitter", "--quiet")
    check("unknown --bake map is exit 2", r.returncode == 2, str(r.returncode))
    check("unknown --bake map is named", "glitter" in r.stderr, r.stderr)

    print(f"\n{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
