#!/usr/bin/env python3
"""Bake provider binding test (engine-bindings, pipeline-bridge).

The seam an external map consumer drives, from the language this repository's
integration suite is written in. Self-skips (exit 77, CTest SKIP) when the
shared library is not loadable, so it is safe to run unconditionally in CI.
"""

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

# Two DISJOINT quads, so the object-id map's face-connected-component fallback
# finds two ids and a one-row id table has something to truncate.
_TWO_ISLAND_PLANE = (
    "v 0 0 0\nv 0.45 0 0\nv 0.45 1 0\nv 0 1 0\n"
    "v 0.55 0 0\nv 1 0 0\nv 1 1 0\nv 0.55 1 0\n"
    "vt 0 0\nvt 0.45 0\nvt 0.45 1\nvt 0 1\n"
    "vt 0.55 0\nvt 1 0\nvt 1 1\nvt 0.55 1\n"
    "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
    "f 5/5 6/6 7/7\nf 5/5 7/7 8/8\n"
)


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE — reported as Skipped, never a vacuous pass

    try:
        import numpy  # noqa: F401  (the pixel comparison below uses ndarrays)
    except ImportError:
        print("SKIP: numpy not available")
        return 77

    from cyberremesh import (
        BakeMap, BakeParams, CyberError, EncodingBasis, Mesh,
        bake, bake_provider_bake, bake_provider_map_list, bake_provider_maps,
        bake_provider_size, find_bake_provider_map,
    )

    obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    obj.write(_UV_PLANE)
    obj.close()
    try:
        with Mesh.load_obj(obj.name) as low, Mesh.load_obj(obj.name) as high:
            # --- the capability query --------------------------------------
            maps = bake_provider_maps()
            assert maps, "this build advertises no maps at all"
            names = [entry.name for entry in maps]
            assert len(set(names)) == len(names), names
            for entry in maps:
                assert entry.channels in (1, 3), entry
                assert entry.color_space in ("linear", "srgb"), entry
                # A host reads this to decide whether to put a transfer curve
                # on the texels. "srgb" on a normal, a distance or an id key
                # gamma-encodes DATA; "linear" on colour ships it washed out.
                assert entry.color_space == (
                    "srgb" if entry.name == "color" else "linear"), entry
                assert find_bake_provider_map(entry.name).map == entry.map
                assert entry.name in bake_provider_map_list(), entry.name

            field_only = bake_provider_map_list(field_only=True)
            for entry in maps:
                assert (entry.name in field_only) == entry.field_capable, entry

            try:
                find_bake_provider_map("no-such-map")
                raise AssertionError("an unknown map name was accepted")
            except CyberError as exc:
                assert "no-such-map" in str(exc), exc
                assert "normal" in str(exc), exc

            params = BakeParams(width=8, height=8)

            # --- the sizing call --------------------------------------------
            sized = bake_provider_size(low, BakeMap.NORMAL, params, high=high)
            assert sized.pixel_count == 8 * 8 * 3, sized
            assert sized.texels_covered == 0, "the sizing call must not bake"

            # --- request every advertised map -------------------------------
            for entry in maps:
                pixels, result = bake_provider_bake(low, entry.map, params, high=high)
                assert result.channels == entry.channels, (entry, result)
                assert result.width == 8 and result.height == 8, result
                assert result.texels_covered > 0, (entry, result)
                # The green-channel convention travels with the pixels rather
                # than being documented out of band.
                assert result.normal_green_plus_y == 1, result
                assert result.padding.radius == params.padding_radius, result
                assert len(pixels.reshape(-1)) == result.pixel_count, entry

            # --- the provider takes the SHARED bake path --------------------
            pixels, result = bake_provider_bake(
                low, BakeMap.OBJECT_POSITION, params, high=high)
            with bake(low, high, BakeMap.OBJECT_POSITION, params) as image:
                reference = image.to_numpy()
                assert (pixels == reference).all(), "provider and bake() disagree"
                assert result.encoding.basis == image.encoding.basis
                assert result.encoding.bounds_min == image.encoding.bounds_min
                assert result.encoding.bounds_max == image.encoding.bounds_max
            assert result.encoding.basis == EncodingBasis.OBJECT_BOUNDS, result

            # --- progress ----------------------------------------------------
            seen = []
            bake_provider_bake(low, BakeMap.AO, BakeParams(width=32, height=32,
                                                           ao_samples=8),
                               high=high, progress=lambda f, s: seen.append(f))
            assert len(seen) > 1, "progress reported once is not progress"

            # --- a map this build cannot produce ----------------------------
            try:
                bake_provider_bake(low, 4242, params, high=high)
                raise AssertionError("an unknown map code was accepted")
            except CyberError as exc:
                assert "4242" in str(exc), exc
                assert bake_provider_map_list() in str(exc), exc

            # --- a short id table is a two-call sizing, not a refusal --------
            # The one asymmetry in this surface: a short PIXEL buffer is
            # refused, because the consumer could have computed that count from
            # the capability query first, while the id count is only knowable
            # once the bake has read the Target. So a short id table succeeds,
            # fills what fits, and still reports the TOTAL.
            island_obj = tempfile.NamedTemporaryFile(
                suffix=".obj", delete=False, mode="w")
            island_obj.write(_TWO_ISLAND_PLANE)
            island_obj.close()
            try:
                with Mesh.load_obj(island_obj.name) as islands:
                    _, whole = bake_provider_bake(
                        islands, BakeMap.OBJECT_ID, params, high=islands)
                    assert whole.id_color_count == 2, whole
                    assert len(whole.encoding.id_colors) == 2, whole

                    _, short = bake_provider_bake(
                        islands, BakeMap.OBJECT_ID, params, high=islands,
                        max_id_colors=1)
                    # The TOTAL, so a host learns how much to allocate; and
                    # exactly one row, because that is all it asked room for.
                    assert short.id_color_count == 2, short
                    assert len(short.encoding.id_colors) == 1, short
                    assert short.encoding.id_colors[0] == whole.encoding.id_colors[0]
            finally:
                os.unlink(island_obj.name)

            # --- cancellation -------------------------------------------------
            try:
                bake_provider_bake(low, BakeMap.NORMAL,
                                   BakeParams(width=64, height=64), high=high,
                                   cancel=lambda: True)
                raise AssertionError("a cancelled bake returned a map")
            except CyberError as exc:
                assert "CANCELLED" in str(exc), exc

    finally:
        os.unlink(obj.name)

    print("bake provider binding OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
