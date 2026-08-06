#!/usr/bin/env python3
"""Named export presets through the Python bindings (mesh-io).

Covers the whole preset surface the bindings add: listing the built-ins,
resolving one by name and by path (including the typed schema-version
rejection), reading what it declares, and writing a bundle for a mesh pair.

Self-skips (exit 77, CTest SKIP) when the shared library is not loadable, so it
is safe to run unconditionally in CI.
"""

import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

# A UV'd plane as the low-poly (so the bundle does not have to unwrap it) and
# the same plane lifted, as the projection source.
_UV_PLANE = (
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
    "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
)
_PLAIN_PLANE = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n"
_LIFTED_PLANE = (
    "v 0 0 0.05\nv 1 0 0.05\nv 1 1 0.05\nv 0 1 0.05\nf 1 2 3\nf 1 3 4\n"
)


_REPO = os.path.dirname(  # <repo>/python/cyberremesh/tests -> <repo>
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)


def _read(path: str) -> str:
    with open(os.path.join(_REPO, path), "r", encoding="utf-8") as handle:
        return handle.read()


def check_parity() -> None:
    """Every export-preset / bundle ABI symbol is declared AND reachable.

    Source-level, like the seam-path and soft-selection gates. Swift is NOT
    checked here: the Swift package wraps the interactive iPad surface only —
    it binds neither the atlas, the handoff readers nor the bake entry points —
    and export bundles are a batch surface.
    """
    header = _read("capi/include/cyber_capi.h")
    symbols = sorted(set(re.findall(r"\bcyber_(?:export_preset|export_bundle|bundle_result|"
                                    r"default_bundle_params)[a-z_]*", header)))
    assert len(symbols) >= 15, symbols

    ffi = _read("python/cyberremesh/cyberremesh/_ffi.py")
    api = _read("python/cyberremesh/cyberremesh/api.py")
    for symbol in symbols:
        assert symbol in ffi, "{0}: no ctypes declaration in _ffi.py".format(symbol)
        assert symbol in api, "{0}: not reachable from api.py".format(symbol)

    # The preset info struct must mirror the header field-for-field.
    fields = [name for name, _ in cyberremesh._ffi.CyberExportPresetInfo._fields_]
    assert fields == [
        "name",
        "schema_version",
        "mesh_format",
        "texture_format",
        "naming_pattern",
        "units",
        "up_axis",
        "resolution",
        "normal_green_plus_y",
        "map_count",
    ], fields
    print("PASS parity: {0} export-preset symbols bound in C ABI / Python".format(
        len(symbols)))


def write(tmpdir: str, name: str, text: str) -> str:
    path = os.path.join(tmpdir, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


def check_builtins() -> None:
    from cyberremesh import ExportPreset, builtin_presets

    names = builtin_presets()
    assert "blender" in names and "unreal" in names, names

    for name in names:
        with ExportPreset.resolve(name) as preset:
            assert preset.name == name, (preset.name, name)
            assert preset.schema_version == 1, preset.schema_version
            assert "." not in preset.mesh_format, preset.mesh_format
            assert preset.texture_format in ("png", "exr"), preset.texture_format
            assert preset.resolution > 0, preset.resolution
            assert preset.maps, name
            # Only Unreal reads DirectX-style normals; everyone else OpenGL.
            expected = "-Y" if name == "unreal" else "+Y"
            assert preset.normal_green == expected, (name, preset.normal_green)
            # Every map expands to a distinct name, or one would overwrite
            # the previous one.
            files = [preset.map_file_name(i, "spot") for i in range(len(preset.maps))]
            assert len(set(files)) == len(files), files
            assert all("{" not in f and "spot" in f for f in files), files
            for entry in preset.maps:
                assert entry.color_space in ("linear", "srgb"), entry
                assert entry.suffix, entry

    with ExportPreset.resolve("blender") as preset:
        preset.resolution = 64
        assert preset.resolution == 64, preset.resolution
        try:
            preset.resolution = 0
        except cyberremesh.CyberError:
            pass
        else:
            raise AssertionError("a non-positive resolution must be rejected")
    print("PASS presets: {0} built-ins resolve, declare distinct map names, and "
          "carry each app's normal convention".format(len(names)))


def check_resolution_errors(tmpdir: str) -> None:
    from cyberremesh import ExportPreset, IncompatibleVersionError

    try:
        ExportPreset.resolve("definitely-not-a-preset")
    except cyberremesh.CyberError as exc:
        # The message names the alternatives rather than just failing.
        assert "blender" in str(exc), str(exc)
    else:
        raise AssertionError("an unknown preset name must raise")

    # A well-formed preset from a FUTURE schema is a contract mismatch, not a
    # parse error: rejected loudly, never partially honored.
    future = write(tmpdir, "future.json", json.dumps(
        {"schemaVersion": 99, "name": "future", "maps": [{"map": "normal"}]}))
    try:
        ExportPreset.resolve(future)
    except IncompatibleVersionError as exc:
        assert "99" in str(exc), str(exc)
    else:
        raise AssertionError("an unsupported preset schema must raise the typed error")

    # A user preset from a file resolves exactly like a built-in.
    custom = write(tmpdir, "custom.json", json.dumps({
        "schemaVersion": 1,
        "name": "studio",
        "meshFormat": "obj",
        "namingPattern": "{basename}-{map}-{preset}.{ext}",
        "normalGreen": "-Y",
        "resolution": 32,
        "maps": [{"map": "normal"}, {"map": "ao", "suffix": "occ"}],
    }))
    with ExportPreset.resolve(custom) as preset:
        assert preset.name == "studio", preset.name
        assert preset.normal_green == "-Y", preset.normal_green
        assert preset.resolution == 32, preset.resolution
        assert [m.map for m in preset.maps] == ["normal", "ao"], preset.maps
        # The suffix override is what {map} expands to, not the map's own name.
        assert preset.map_file_name(1, "head") == "head-occ-studio.png", \
            preset.map_file_name(1, "head")
    print("PASS presets: unknown name lists the built-ins, schema 99 raises "
          "IncompatibleVersionError, a user preset file resolves")


def check_bundle(tmpdir: str) -> None:
    from cyberremesh import ExportPreset, Mesh, write_bundle

    low_path = write(tmpdir, "low.obj", _UV_PLANE)
    high_path = write(tmpdir, "high.obj", _LIFTED_PLANE)
    out_dir = os.path.join(tmpdir, "bundle")
    os.makedirs(out_dir, exist_ok=True)

    stages = []
    with Mesh.load_obj(low_path) as low, Mesh.load_obj(high_path) as high:
        with ExportPreset.resolve("blender") as preset:
            preset.resolution = 16
            result = write_bundle(
                low, high, preset, os.path.join(out_dir, "plane.obj"),
                ao_samples=4, cage_distance=0.2,
                progress=lambda fraction, stage: stages.append(stage),
            )
            map_count = len(preset.maps)

    assert stages, "the progress callback never fired"
    # The low-poly already had UVs, so nothing was unwrapped.
    assert not result.unwrapped, result
    assert result.chart_count == 0, result.chart_count
    assert len(result.files) == map_count + 1, result.files  # mesh + one per map
    assert result.file("mesh") is not None, result.files
    assert result.file("normal") is not None, result.files
    assert result.file("nope") is None, result.files
    for entry in result.files:
        assert os.path.exists(entry.path), entry
        if entry.kind == "mesh":
            assert entry.width == 0, entry
        else:
            assert (entry.width, entry.height) == (16, 16), entry
            assert entry.color_space in ("linear", "srgb"), entry
    print("PASS bundle: blender preset wrote {0} files at the overridden 16x16, "
          "unwrapped nothing".format(len(result.files)))


def check_bundle_unwraps_and_warns(tmpdir: str) -> None:
    from cyberremesh import ExportPreset, Mesh, write_bundle

    # No vt: baking is impossible without UVs, so the bundle unwraps in place.
    low_path = write(tmpdir, "nouv.obj", _PLAIN_PLANE)
    out_dir = os.path.join(tmpdir, "bundle_nouv")
    os.makedirs(out_dir, exist_ok=True)

    with Mesh.load_obj(low_path) as low, Mesh.load_obj(low_path) as high:
        with ExportPreset.resolve("gltf-generic") as preset:
            preset.resolution = 16
            # The preset declares glb; the explicit .ply path is the user
            # speaking last and must win, with the mismatch reported.
            result = write_bundle(low, high, preset,
                                  os.path.join(out_dir, "plane.ply"),
                                  basename="custom", ao_samples=4,
                                  cage_distance=0.2)

    assert result.unwrapped, result
    assert result.chart_count > 0, result.chart_count
    assert result.max_angle_distortion >= 0.0, result.max_angle_distortion
    assert any("ply" in w for w in result.warnings), result.warnings
    for entry in result.files:
        if entry.kind != "mesh":
            assert os.path.basename(entry.path).startswith("custom"), entry
    print("PASS bundle: an unwrapped low-poly reports {0} charts, and the "
          "preset/extension mismatch surfaces as a warning".format(result.chart_count))


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE — reported as Skipped, never a vacuous pass

    tmpdir = tempfile.mkdtemp(prefix="cyber_py_presets_")
    check_parity()
    check_builtins()
    check_resolution_errors(tmpdir)
    try:
        check_bundle(tmpdir)
        check_bundle_unwraps_and_warns(tmpdir)
    except cyberremesh.CyberError as exc:
        # A build without the UV module has the preset DATA but no bundle
        # writer; that is a configuration, not a failure.
        if "export-bundle module" not in str(exc):
            raise
        print("SKIP bundle: this build has no export-bundle module (CYBER_BUILD_UV=OFF)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
