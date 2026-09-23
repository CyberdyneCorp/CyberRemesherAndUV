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

        # Two DIFFERENT refusals reach the same NULL return: an out-of-range
        # index, and a basename whose expansion would leave the output
        # directory. The binding used to report both as "map index out of
        # range", which sent anyone hitting the second one looking at their
        # loop counter instead of at their basename.
        try:
            preset.map_file_name(len(preset.maps), "head")
        except cyberremesh.CyberError as exc:
            assert "index" in str(exc), str(exc)
        else:
            raise AssertionError("an out-of-range map index must raise")

        try:
            preset.map_file_name(0, "../../escape")
        except cyberremesh.CyberError as exc:
            message = str(exc)
            assert "index out of range" not in message, message
            assert "outside the output directory" in message, message
        else:
            raise AssertionError("an escaping basename must raise")
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


def check_bundle_regioned(tmpdir: str) -> None:
    """A working-set bound streams every map in regions; the bytes do not change."""
    from cyberremesh import ExportPreset, Mesh, write_bundle

    low_path = write(tmpdir, "rlow.obj", _UV_PLANE)
    high_path = write(tmpdir, "rhigh.obj", _LIFTED_PLANE)
    whole_dir = os.path.join(tmpdir, "whole")
    band_dir = os.path.join(tmpdir, "band")
    os.makedirs(whole_dir, exist_ok=True)
    os.makedirs(band_dir, exist_ok=True)
    with Mesh.load_obj(low_path) as low, Mesh.load_obj(high_path) as high:
        with ExportPreset.resolve("blender") as preset:
            preset.resolution = 64
            whole = write_bundle(low, high, preset, os.path.join(whole_dir, "plane.obj"),
                                 ao_samples=4, cage_distance=0.2)
            band = write_bundle(low, high, preset, os.path.join(band_dir, "plane.obj"),
                                ao_samples=4, cage_distance=0.2,
                                max_working_set_texels=64 * 40)
    assert len(whole.files) == len(band.files), (whole.files, band.files)
    for a, b in zip(whole.files, band.files):
        if a.kind == "mesh":
            assert b.regions.count == 0, b.regions
            continue
        with open(a.path, "rb") as fa, open(b.path, "rb") as fb:
            assert fa.read() == fb.read(), (a.path, b.path)
        assert a.regions.count == 1, a.regions
        assert (b.regions.count, b.regions.rows, b.regions.halo_rows) == (8, 8, 16), b.regions
        assert b.regions.working_set_texels == 64 * 40, b.regions
    print("PASS bundle: a working-set bound streams the same bytes in 8 regions")


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


def check_bundle_id_table(tmpdir: str) -> None:
    """The id table of a bundled map, read back through the binding.

    write_bundle reads the table through a different C surface than a direct
    bake does — CyberImageEncoding is a flat POD, so the bundle keeps its own
    copy behind cyber_bundle_result_file_id_* — and that surface is what a
    host writing an export gets. A dropped or reordered row there leaves the
    written PNG unresolvable even though bake() itself is fine.
    """
    from cyberremesh import EncodingBasis, ExportPreset, Mesh, write_bundle, bake
    from cyberremesh import BakeMap, BakeParams

    low_path = write(tmpdir, "id_low.obj", _UV_PLANE)
    # sRGB on an id map is the trap: a gamma curve rewrites every id's colour,
    # so the bundle must refuse it and report the refusal.
    preset_path = write(tmpdir, "id_preset.json", json.dumps({
        "schemaVersion": 1,
        "name": "idmaps",
        "meshFormat": "obj",
        "resolution": 16,
        "maps": [{"map": "material-id", "colorSpace": "srgb"}, {"map": "object-id"}],
    }))
    out_dir = os.path.join(tmpdir, "bundle_ids")
    os.makedirs(out_dir, exist_ok=True)

    positions = [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]
    offsets = [0, 3, 6]
    indices = [0, 1, 2, 0, 2, 3]
    with Mesh.load_obj(low_path) as low, Mesh.from_indexed(
        positions, offsets, indices, {("face", "material_id"): [4, 9]}
    ) as high:
        with ExportPreset.resolve(preset_path) as preset:
            result = write_bundle(low, high, preset,
                                  os.path.join(out_dir, "plane.obj"),
                                  ao_samples=4, cage_distance=0.2)
        # The same pair baked directly: the bundle has to report the very
        # same table, or a colour picked out of the written file resolves to
        # a different id than the bake assigned it.
        with bake(low, high, BakeMap.MATERIAL_ID,
                  BakeParams(width=16, height=16, cage_distance=0.2)) as img:
            direct = img.encoding

    material = result.file("material-id")
    assert material is not None, result.files
    assert material.encoding.basis == EncodingBasis.ID_COLOR, material.encoding
    assert material.encoding.id_source == "material_id", material.encoding
    assert material.encoding.id_source == direct.id_source, material.encoding
    assert [row.id for row in material.encoding.id_colors] == [4, 9], material.encoding
    assert list(material.encoding.id_colors) == list(direct.id_colors), (
        material.encoding.id_colors, direct.id_colors)
    for row in material.encoding.id_colors:
        assert row.color != (0, 0, 0) and min(row.color) >= 64, row
    # Written linear despite the sRGB request, and the refusal is reported.
    assert material.color_space == "linear", material
    assert any("material-id" in w for w in result.warnings), result.warnings

    # No column for the object map: it falls back to components and says so.
    obj = result.file("object-id")
    assert obj is not None, result.files
    assert obj.encoding.id_source == "component", obj.encoding
    assert len(obj.encoding.id_colors) == 1, obj.encoding

    # The mesh entry is not a map: no source, no table.
    mesh_entry = result.file("mesh")
    assert mesh_entry is not None, result.files
    assert mesh_entry.encoding.id_source == "", mesh_entry.encoding
    assert mesh_entry.encoding.id_colors == (), mesh_entry.encoding
    print("PASS bundle: the id maps carry the same table the direct bake reports, "
          "and the sRGB request is refused")


def check_bundle_udim(tmpdir: str) -> None:
    """One file per occupied tile, and the refusal when the pattern names none.

    The preset the bundle is given here has an explicit `{udim}` token; the
    default patterns carry none, which is exactly the case the second half
    checks, because every tile would otherwise be written to one path.
    """
    from cyberremesh import ExportPreset, Mesh, udim_tiles, write_bundle

    two_tiles = (
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "v 3 0 0\nv 4 0 0\nv 4 1 0\nv 3 1 0\n"
        "vt 0.1 0.1\nvt 0.9 0.1\nvt 0.9 0.9\nvt 0.1 0.9\n"
        "vt 1.1 0.1\nvt 1.9 0.1\nvt 1.9 0.9\nvt 1.1 0.9\n"
        "f 1/1 2/2 3/3 4/4\nf 5/5 6/6 7/7 8/8\n"
    )
    low_path = write(tmpdir, "udim_low.obj", two_tiles)
    out_dir = os.path.join(tmpdir, "bundle_udim")
    os.makedirs(out_dir, exist_ok=True)
    preset_path = write(tmpdir, "udim_preset.json", json.dumps({
        "schemaVersion": 1, "name": "udim", "resolution": 16,
        "namingPattern": "{basename}.{map}.{udim}.{ext}", "maps": ["curvature"],
    }))

    with Mesh.load_obj(low_path) as low, Mesh.load_obj(low_path) as high:
        assert udim_tiles(low).tiles == (1001, 1002), udim_tiles(low).tiles
        with ExportPreset.resolve(preset_path) as preset:
            result = write_bundle(low, high, preset, os.path.join(out_dir, "hero.obj"),
                                  cage_distance=0.2, udim=True)
    # The layout query is `udim_tiles` above -- the bundle result does not
    # duplicate it; what it adds is which tile each FILE holds.
    tiles = sorted(entry.udim_tile for entry in result.files if entry.width > 0)
    assert tiles == [1001, 1002], tiles
    assert os.path.exists(os.path.join(out_dir, "hero.curvature.1001.png")), os.listdir(out_dir)
    assert os.path.exists(os.path.join(out_dir, "hero.curvature.1002.png")), os.listdir(out_dir)

    # The same layout through a pattern that names no tile: refused, not
    # silently collapsed onto one path.
    refused_dir = os.path.join(tmpdir, "bundle_udim_refused")
    os.makedirs(refused_dir, exist_ok=True)
    with Mesh.load_obj(low_path) as low, Mesh.load_obj(low_path) as high:
        with ExportPreset.resolve("blender") as preset:
            preset.resolution = 16
            try:
                write_bundle(low, high, preset, os.path.join(refused_dir, "hero.obj"),
                             cage_distance=0.2, udim=True)
                raise AssertionError("a multi-tile bundle without {udim} was not refused")
            except cyberremesh.CyberError as error:
                assert "{udim}" in str(error), str(error)
    assert not os.path.exists(os.path.join(refused_dir, "hero.obj")), os.listdir(refused_dir)

    # The HOST's texel ceiling reaches this path too. A UDIM bundle multiplies
    # the exposure by the occupied-tile count, so a ceiling that bounds one map
    # has to bound the set -- and the refusal has to say which of the two it hit.
    ceiling_dir = os.path.join(tmpdir, "bundle_udim_ceiling")
    os.makedirs(ceiling_dir, exist_ok=True)
    previous = cyberremesh.max_bake_pixels()
    try:
        # 16x16 is 256 texels: one tile fits under 257, two do not.
        cyberremesh.set_max_bake_pixels(257)
        with Mesh.load_obj(low_path) as low, Mesh.load_obj(low_path) as high:
            with ExportPreset.resolve(preset_path) as preset:
                try:
                    write_bundle(low, high, preset, os.path.join(ceiling_dir, "hero.obj"),
                                 cage_distance=0.2, udim=True)
                    raise AssertionError("the aggregate ceiling was not applied to the bundle")
                except cyberremesh.CyberError as error:
                    assert "AGGREGATE" in str(error), str(error)
        assert not os.path.exists(os.path.join(ceiling_dir, "hero.obj")), os.listdir(ceiling_dir)

        cyberremesh.set_max_bake_pixels(255)  # below ONE tile: the other diagnosis
        with Mesh.load_obj(low_path) as low, Mesh.load_obj(low_path) as high:
            with ExportPreset.resolve(preset_path) as preset:
                try:
                    write_bundle(low, high, preset, os.path.join(ceiling_dir, "hero.obj"),
                                 cage_distance=0.2, udim=True)
                    raise AssertionError("the per-tile ceiling was not applied to the bundle")
                except cyberremesh.CyberError as error:
                    assert "PER-TILE" in str(error), str(error)
    finally:
        cyberremesh.set_max_bake_pixels(previous)
    print("PASS bundle: one file per occupied tile, a pattern naming no tile is refused, "
          "and both texel ceilings bind")


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
        check_bundle_id_table(tmpdir)
        check_bundle_udim(tmpdir)
        check_bundle_regioned(tmpdir)
    except cyberremesh.CyberError as exc:
        # A build without the UV module has the preset DATA but no bundle
        # writer; that is a configuration, not a failure.
        if "export-bundle module" not in str(exc):
            raise
        print("SKIP bundle: this build has no export-bundle module (CYBER_BUILD_UV=OFF)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
