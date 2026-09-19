#!/usr/bin/env python3
"""Surface-baking binding test. Self-skips (exit 77, CTest SKIP) when the shared library is
not loadable, so it is safe to run unconditionally in CI."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

_UV_PLANE = (
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
    "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
)

# The same plane with its UVs in the lower-left QUARTER of the layout, so three
# quarters of the image is background for a padded band to grow into.
_QUARTER_UV_PLANE = (
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "vt 0 0\nvt 0.5 0\nvt 0.5 0.5\nvt 0 0.5\n"
    "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
)


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE — reported as Skipped, never a vacuous pass

    try:
        import numpy  # noqa: F401  (the pixel checks below use Image.to_numpy())
    except ImportError:
        print("SKIP: numpy not available")
        return 77

    from cyberremesh import BakeMap, BakeParams, Mesh, bake

    obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    obj.write(_UV_PLANE)
    obj.close()
    try:
        with Mesh.load_obj(obj.name) as low, Mesh.load_obj(obj.name) as high:
            img = bake(low, high, BakeMap.NORMAL, BakeParams(width=16, height=16))
            assert img.width == 16 and img.height == 16, (img.width, img.height)
            assert img.channels == 3, img.channels

            arr = img.to_numpy()
            assert arr.shape == (16, 16, 3), arr.shape
            # Coincident flat planes -> tangent-space up -> centre blue channel ~1.
            center_z = float(arr[8, 8, 2])
            assert abs(center_z - 1.0) < 0.05, ("center normal z", center_z)

            png = tempfile.NamedTemporaryFile(suffix=".png", delete=False)
            png.close()
            img.save_png(png.name)
            assert os.path.getsize(png.name) > 0
            os.unlink(png.name)
            img.close()

            # Curvature/cavity reach the bindings and carry the extra
            # curvature_range field across the ctypes struct boundary. A flat
            # Target has nothing to normalize against, so both take their
            # neutral value: mid-gray for curvature, white for cavity.
            params = BakeParams(width=16, height=16, curvature_range=0.0)
            with bake(low, high, BakeMap.CURVATURE, params) as curv:
                assert curv.channels == 1, curv.channels
                mid = float(curv.to_numpy()[8, 8, 0])
                assert abs(mid - 0.5) < 0.02, ("flat curvature", mid)
            with bake(low, high, BakeMap.CAVITY, params) as cav:
                assert cav.channels == 1, cav.channels
                white = float(cav.to_numpy()[8, 8, 0])
                assert abs(white - 1.0) < 0.02, ("flat cavity", white)
        print("PASS bake: normal map points up; curvature/cavity read neutral on a flat Target")

        _gate_the_mesh_map_set(obj.name)
        _gate_the_world_and_density_maps(obj.name)
        _gate_the_id_maps(obj.name)
        _gate_border_padding(obj.name)
        _gate_a_raising_evaluator_raises(obj.name)
        _gate_the_openness_rename_shim(obj.name)
        _gate_udim(obj.name)
    finally:
        os.unlink(obj.name)
    return 0


def _gate_udim(unit_square_obj):
    """UDIM tile detection and the per-tile bake, through the binding.

    The case that matters is the one the C++ suite pins: an occluder whose UVs
    lie in ANOTHER tile still occludes. Here the binding is what is under test,
    so this asserts the surface reaches Python intact -- the tile list without a
    bake, one image per tile in ascending order, and the ceiling refusals naming
    which of the two they hit.
    """
    import tempfile as _t

    two_tiles = (
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "v 3 0 0\nv 4 0 0\nv 4 1 0\nv 3 1 0\n"
        "vt 0.1 0.1\nvt 0.9 0.1\nvt 0.9 0.9\nvt 0.1 0.9\n"
        "vt 1.1 0.1\nvt 1.9 0.1\nvt 1.9 0.9\nvt 1.1 0.9\n"
        "f 1/1 2/2 3/3 4/4\nf 5/5 6/6 7/7 8/8\n"
    )
    handle = _t.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    handle.write(two_tiles)
    handle.close()
    try:
        with cyberremesh.Mesh.load_obj(handle.name) as low:
            layout = cyberremesh.udim_tiles(low)
            assert layout.tiles == (1001, 1002), layout.tiles
            assert layout.unaddressable_faces == 0, layout.unaddressable_faces

            with cyberremesh.Mesh.load_obj(unit_square_obj) as square:
                assert cyberremesh.udim_tiles(square).tiles == (1001,)

            with cyberremesh.Mesh.load_obj(handle.name) as high:
                params = cyberremesh.BakeParams(width=16, height=16)
                cyberremesh.set_max_bake_pixels(0)
                tiles = cyberremesh.bake_udim(low, high, cyberremesh.BakeMap.NORMAL, params)
                try:
                    assert [t.tile for t in tiles] == [1001, 1002], [t.tile for t in tiles]
                    for entry in tiles:
                        assert entry.image.width == 16, entry.image.width
                        assert entry.image.channels == 3, entry.image.channels
                finally:
                    for entry in tiles:
                        entry.image.close()

                # One tile of 16x16 is 256 texels: 255 refuses the TILE, 300
                # fits one tile and not two. Two problems, two messages.
                for ceiling, word in ((255, "PER-TILE"), (300, "AGGREGATE")):
                    cyberremesh.set_max_bake_pixels(ceiling)
                    try:
                        cyberremesh.bake_udim(low, high, cyberremesh.BakeMap.NORMAL, params)
                        raise AssertionError(f"ceiling {ceiling} was not refused")
                    except cyberremesh.CyberError as error:
                        assert word in str(error), (ceiling, str(error))
                cyberremesh.set_max_bake_pixels(0)
    finally:
        os.unlink(handle.name)
    print("PASS bake_udim: the tile list, the per-tile set and both ceiling refusals bind")


def _gate_the_world_and_density_maps(obj_path):
    """The world-space direction and UV density maps through the binding.

    The world map's identity case is the interesting one: this engine has ONE
    model space, so with an identity placement it must reproduce the
    object-space normal map EXACTLY. Anything else would mean the binding is
    quietly applying something.
    """
    import numpy as np

    from cyberremesh import (BakeMap, BakeParams, DensityNormalization, EncodingBasis,
                             IDENTITY_PLACEMENT, Mesh, PaddingMode, bake)

    params = BakeParams(width=16, height=16, padding_radius=0)
    assert params.placement == IDENTITY_PLACEMENT, params.placement
    assert params.density_normalization == DensityNormalization.ABSOLUTE

    with Mesh.load_obj(obj_path) as low, Mesh.load_obj(obj_path) as high:
        with bake(low, high, BakeMap.WORLD_DIRECTION, params) as world, \
                bake(low, high, BakeMap.OBJECT_NORMAL, params) as obj:
            assert world.encoding.basis == EncodingBasis.WORLD_DIRECTION, world.encoding.basis
            assert np.array_equal(world.to_numpy(), obj.to_numpy()), "identity must be exact"
            assert world.placement == IDENTITY_PLACEMENT, world.placement

        # A quarter turn about X (row-major): y -> z, z -> -y. The plane's
        # normal is +z, so the world direction becomes -y.
        turned = BakeParams(width=16, height=16, padding_radius=0, placement=(
            1, 0, 0, 0,
            0, 0, -1, 0,
            0, 1, 0, 0,
            0, 0, 0, 1,
        ))
        with bake(low, high, BakeMap.WORLD_DIRECTION, turned) as rotated:
            decoded = rotated.to_numpy()[8, 8] * 2.0 - 1.0
            assert abs(decoded[0]) < 1e-3, decoded
            assert abs(decoded[1] + 1.0) < 1e-3, decoded
            assert abs(decoded[2]) < 1e-3, decoded
            assert rotated.placement == turned.placement, rotated.placement

        # A placement that cannot carry a direction is refused, not defaulted.
        singular = BakeParams(width=8, height=8, placement=(
            1, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        ))
        try:
            bake(low, high, BakeMap.WORLD_DIRECTION, singular).close()
            raise AssertionError("a singular placement must be refused")
        except cyberremesh.CyberError:
            pass

        # A placement list of the wrong length is refused by the binding itself,
        # rather than padded into a different matrix.
        try:
            BakeParams(placement=(1, 0, 0))._to_c()
            raise AssertionError("a short placement must be refused")
        except ValueError:
            pass

        # The density map: one channel, texels per square model unit. This plane
        # is 1 unit square over the whole layout, so an absolute density at
        # 16x16 is 256.
        with bake(low, high, BakeMap.UV_DENSITY, params) as density:
            assert density.channels == 1, density.channels
            assert density.encoding.basis == EncodingBasis.UV_DENSITY
            assert density.density.normalization == DensityNormalization.ABSOLUTE
            value = float(density.to_numpy()[8, 8, 0])
            assert abs(value - 256.0) < 1.0, value
            assert abs(density.density.mean - 256.0) < 1.0, density.density.mean

        relative = BakeParams(width=16, height=16, padding_radius=0,
                              density_normalization=DensityNormalization.RELATIVE)
        with bake(low, high, BakeMap.UV_DENSITY, relative) as scaled:
            assert scaled.density.normalization == DensityNormalization.RELATIVE
            # Uniformly packed, so every defined texel is exactly its own mean.
            assert abs(float(scaled.to_numpy()[8, 8, 0]) - 1.0) < 1e-3
            # The ABSOLUTE mean travels with the relative map, which is what
            # makes it convertible back.
            assert abs(scaled.density.mean - 256.0) < 1.0, scaled.density.mean

        # The padding record reaches both maps. This plane covers the whole
        # layout, so no band is grown and the rule is NONE -- what is asserted
        # here is that the radius travelled and the density map was not handed a
        # direction map's rule.
        padded = BakeParams(width=16, height=16, padding_radius=4)
        with bake(low, high, BakeMap.WORLD_DIRECTION, padded) as world_padded:
            assert world_padded.padding.radius == 4, world_padded.padding.radius
        with bake(low, high, BakeMap.UV_DENSITY, padded) as density_padded:
            assert density_padded.padding.mode in (
                PaddingMode.NONE, PaddingMode.EXTRAPOLATE), density_padded.padding.mode

    print("PASS bake: world direction follows the placement; uv density reports its mode")


def _gate_the_mesh_map_set(obj_path):
    """The four CyberTexel maps, and the basis that makes them interpretable.

    An object-space position map is `(p - min) / (max - min)`: without `min`
    and `max` a consumer cannot recover a single coordinate, so the binding has
    to carry the basis, not just the pixels.
    """
    from cyberremesh import (BakeMap, BakeParams, BentNormalSpace, EncodingBasis, Mesh,
                             UpAxis, bake)

    with Mesh.load_obj(obj_path) as low, Mesh.load_obj(obj_path) as high:
        params = BakeParams(width=16, height=16, ao_samples=8)
        with bake(low, high, BakeMap.OBJECT_NORMAL, params) as img:
            assert img.channels == 3, img.channels
            # The Target normal is +z, which encodes to (0.5, 0.5, 1).
            arr = img.to_numpy()
            assert abs(float(arr[8, 8, 2]) - 1.0) < 0.05, float(arr[8, 8, 2])
            assert img.encoding.basis == EncodingBasis.OBJECT_NORMAL, img.encoding
            assert img.encoding.up_axis == UpAxis.Y, img.encoding

        # The up axis is applied, not merely declared: z-up re-expresses
        # (x, y, z) as (x, -z, y), so +z lands in the green channel as 0.
        z_up = BakeParams(width=16, height=16, ao_samples=8, up_axis=UpAxis.Z)
        with bake(low, high, BakeMap.OBJECT_NORMAL, z_up) as img:
            assert abs(float(img.to_numpy()[8, 8, 1])) < 0.05, float(img.to_numpy()[8, 8, 1])
            assert img.encoding.up_axis == UpAxis.Z, img.encoding

        with bake(low, high, BakeMap.OBJECT_POSITION, params) as img:
            encoding = img.encoding
            assert encoding.basis == EncodingBasis.OBJECT_BOUNDS, encoding
            assert encoding.bounds_min[0] == 0.0 and encoding.bounds_max[0] == 1.0, encoding
            # Decoding with the recorded basis recovers the model coordinate.
            arr = img.to_numpy()
            span = encoding.bounds_max[0] - encoding.bounds_min[0]
            x = encoding.bounds_min[0] + float(arr[8, 12, 0]) * span
            assert abs(x - 0.78) < 0.08, x

        with bake(low, high, BakeMap.BENT_NORMAL, params) as img:
            assert img.channels == 3, img.channels
            assert img.encoding.basis == EncodingBasis.TANGENT_NORMAL, img.encoding
        object_bent = BakeParams(width=16, height=16, ao_samples=8,
                                 bent_normal_space=BentNormalSpace.OBJECT)
        with bake(low, high, BakeMap.BENT_NORMAL, object_bent) as img:
            assert img.encoding.basis == EncodingBasis.OBJECT_NORMAL, img.encoding

        thick = BakeParams(width=16, height=16, ao_samples=8, thickness_scale=3.0)
        with bake(low, high, BakeMap.THICKNESS, thick) as img:
            assert img.channels == 1, img.channels
            assert img.encoding.basis == EncodingBasis.DISTANCE, img.encoding
            assert abs(img.encoding.scale - 3.0) < 1e-6, img.encoding
            # A single quad has no interior, so its inverted-normal rays escape.
            assert float(img.to_numpy()[8, 8, 0]) == 0.0, float(img.to_numpy()[8, 8, 0])

        # Out of range is refused at the binding's entry point, not defaulted.
        for bad in (BakeParams(width=16, height=16, up_axis=7),
                    BakeParams(width=16, height=16, bent_normal_space=-3),
                    BakeParams(width=16, height=16, thickness_scale=-1.0),
                    BakeParams(width=16, height=16, padding_radius=-1)):
            try:
                bake(low, high, BakeMap.OBJECT_NORMAL, bad).close()
            except cyberremesh.CyberError:
                pass
            else:
                raise AssertionError("an out-of-range encoding parameter was accepted")

    print("PASS bake: the object-space and ray-traced maps carry a decodable encoding basis")


def _gate_border_padding(obj_path):
    """The padding record reaches the binding, and the radius reaches the bake.

    The low-poly's UVs are shrunk into a quarter of the layout, so there IS a
    band; without that the map covers the image and the honest answer is "no
    padding", which would let a broken binding pass.
    """
    from cyberremesh import (BakeMap, BakeParams, Mesh, PaddingMode, bake)

    assert BakeParams().padding_radius == 8, BakeParams().padding_radius
    quarter = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    quarter.write(_QUARTER_UV_PLANE)
    quarter.close()
    try:
        _padding_checks(quarter.name, obj_path)
    finally:
        os.unlink(quarter.name)

    print("PASS bake: the padding record reaches the binding and the radius reaches the bake")


def _padding_checks(low_path, obj_path):
    from cyberremesh import (BakeMap, BakeParams, Mesh, PaddingMode, bake)

    with Mesh.load_obj(low_path) as low, Mesh.load_obj(obj_path) as high:
        padded = BakeParams(width=32, height=32, padding_radius=6)
        with bake(low, high, BakeMap.NORMAL, padded) as img:
            record = img.padding
            assert record.radius == 6, record
            assert record.mode == PaddingMode.EXTRAPOLATE_UNIT, record
            assert record.texels_filled > 0, record
        with bake(low, high, BakeMap.MATERIAL_ID, padded) as img:
            # An id map is copied, never interpolated -- the binding has to
            # show that, because it is the difference between a usable key map
            # and an unusable one.
            assert img.padding.mode == PaddingMode.NEAREST, img.padding
        off = BakeParams(width=32, height=32, padding_radius=0)
        with bake(low, high, BakeMap.NORMAL, off) as img:
            assert img.padding.radius == 0, img.padding
            assert img.padding.mode == PaddingMode.NONE, img.padding
            assert img.padding.texels_filled == 0, img.padding


def _gate_the_id_maps(obj_path):
    """A colour-ID map is only usable with the table that resolves its colours.

    So this checks the two together: every non-padding texel in the image must
    be a colour the reported table names, byte for byte. Anything the binding
    dropped, reordered or rounded would break that join, and a picked colour
    would resolve to nothing.
    """
    from cyberremesh import BakeMap, BakeParams, EncodingBasis, Mesh, bake

    positions = [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]
    offsets = [0, 3, 6]
    indices = [0, 1, 2, 0, 2, 3]
    params = BakeParams(width=16, height=16)
    with Mesh.load_obj(obj_path) as low, Mesh.from_indexed(
        positions, offsets, indices, {("face", "material_id"): [4, 9]}
    ) as high:
        with bake(low, high, BakeMap.MATERIAL_ID, params) as img:
            encoding = img.encoding
            assert encoding.basis == EncodingBasis.ID_COLOR, encoding
            assert encoding.id_source == "material_id", encoding
            # Ascending by id, not the order the faces declared them in.
            assert [row.id for row in encoding.id_colors] == [4, 9], encoding.id_colors
            for row in encoding.id_colors:
                assert row.color != (0, 0, 0) and min(row.color) >= 64, row

            arr = img.to_numpy()
            table = {row.color for row in encoding.id_colors}
            seen = set()
            for y in range(arr.shape[0]):
                for x in range(arr.shape[1]):
                    px = tuple(int(round(float(arr[y, x, c]) * 255.0)) for c in range(3))
                    assert px == (0, 0, 0) or px in table, (x, y, px)
                    seen.add(px)
            assert table <= seen, (sorted(table), sorted(seen))

        # No declared column: the object map falls back to components and says so.
        with bake(low, high, BakeMap.OBJECT_ID, params) as img:
            assert img.encoding.id_source == "component", img.encoding
            assert len(img.encoding.id_colors) == 1, img.encoding

        # Every other map reports no table at all.
        with bake(low, high, BakeMap.NORMAL, params) as img:
            assert img.encoding.id_source == "", img.encoding
            assert img.encoding.id_colors == (), img.encoding

    print("PASS bake: the id maps carry a table that resolves every texel they wrote")


class _RaisingField(cyberremesh.FieldEvaluator):
    """An evaluator whose distance() raises, which host code does by accident."""

    def distance(self, p):
        raise ValueError("the host's SDF blew up")

    def gradient(self, p):
        return (0.0, 0.0, 1.0)

    def openness(self, p, n, radius):
        return 1.0


def _gate_a_raising_evaluator_raises(obj_path):
    """An exception in a callback must surface, not become a plausible map.

    A Python exception cannot cross the C boundary, so the trampolines used to
    swallow it and substitute a value: 0.0 for distance, (0,0,1) for gradient,
    1.0 for occlusion. Every one of those is PLAUSIBLE, and 0.0 is worse than
    plausible -- |0.0| <= the tracer's epsilon, so a distance callback that
    raised reported a HIT at the cage origin and bake_field returned an image
    that looked measured. The substitute is NaN now, which the engine's field
    guard rejects, and the original exception is re-raised once the C call has
    unwound.
    """
    field = _RaisingField()
    params = cyberremesh.BakeParams(width=8, height=8)
    with cyberremesh.Mesh.load(obj_path) as low:
        try:
            cyberremesh.bake_field(low, cyberremesh.BakeMap.NORMAL, field, params)
        except ValueError as exc:
            assert "blew up" in str(exc), str(exc)
        else:
            raise AssertionError("a raising field evaluator produced an image")
    # The slot is cleared, so a later bake with a working field is not poisoned
    # by the previous failure.
    assert field._pending_error is None
    print("PASS bake_field: a raising evaluator raises instead of baking a fabricated map")


class _OldNameField(cyberremesh.FieldEvaluator):
    """Written against the pre-rename API: defines `occlusion`, means openness."""

    def distance(self, p):
        return p[2]

    def gradient(self, p):
        return (0.0, 0.0, 1.0)

    def occlusion(self, p, n, radius):
        return 0.25


class _NewNameField(_OldNameField):
    def openness(self, p, n, radius):
        return 0.25


def _gate_the_openness_rename_shim(obj_path):
    """`occlusion` was renamed to `openness`, and the shim must FORWARD not invert.

    The old name described the inverse of the value it returned, which is the
    whole footgun: an implementer following the name computed occlusion and
    baked a plausible inverted map. But a subclass that already said
    `occlusion` was returning OPENNESS regardless -- the rename changed the
    name, not the quantity -- so the shim forwards. Inverting here would flip a
    map that was correct before, which is the same bug in the other direction.
    """
    import warnings as _w

    params = cyberremesh.BakeParams(width=8, height=8)
    with cyberremesh.Mesh.load(obj_path) as low:
        with _w.catch_warnings(record=True) as caught:
            _w.simplefilter("always")
            with cyberremesh.bake_field(
                low, cyberremesh.BakeMap.AO, _OldNameField(), params
            ) as old_img:
                old_value = float(old_img.to_numpy()[4, 4, 0])
        assert any(issubclass(c.category, DeprecationWarning) for c in caught), [
            str(c.message) for c in caught
        ]
        with cyberremesh.bake_field(
            low, cyberremesh.BakeMap.AO, _NewNameField(), params
        ) as new_img:
            new_value = float(new_img.to_numpy()[4, 4, 0])

    # Same value from both names: forwarded, not inverted.
    assert abs(old_value - new_value) < 1e-6, (old_value, new_value)
    assert abs(old_value - 0.25) < 0.02, old_value
    print("PASS bake_field: the occlusion->openness shim forwards and warns")


if __name__ == "__main__":
    raise SystemExit(main())
