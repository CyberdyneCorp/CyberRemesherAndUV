#!/usr/bin/env python3
"""Bake provider: the seam an external map consumer drives.

This repository bakes; a texture-painting stage such as CyberTexel consumes the
maps. The seam between them is an INTERFACE, not a dependency — the same shape
as ``CyberFieldEvaluator`` pointing the other way. This example is a consumer,
written against nothing but the bindings:

  1. ask what this build can produce (nothing is hard-coded),
  2. size a request without baking it,
  3. request maps of several encoding bases and read their metadata,
  4. watch progress and cancel a bake,
  5. get refused, by name, for a map that cannot be produced.

Every map it asks for comes back with the up axis, encoding basis, normal
green-channel convention, padding record and id table it needs to interpret the
pixels — rather than with a comment somewhere saying what they mean.
"""

import os
import tempfile

import common as c
from cyberremesh import (
    BakeMap,
    BakeParams,
    CyberError,
    EncodingBasis,
    Mesh,
    bake_provider_bake,
    bake_provider_map_list,
    bake_provider_maps,
    bake_provider_size,
)

_BASIS_NAMES = {
    EncodingBasis.NONE: "raw values",
    EncodingBasis.TANGENT_NORMAL: "direction, tangent frame",
    EncodingBasis.OBJECT_NORMAL: "direction, object space",
    EncodingBasis.OBJECT_BOUNDS: "position rescaled over the bake bounds",
    EncodingBasis.DISTANCE: "a length in model units",
    EncodingBasis.ID_COLOR: "an EXACT id key",
    EncodingBasis.WORLD_DIRECTION: "direction, world space",
    EncodingBasis.UV_DENSITY: "texels per square model unit",
}

# One map per encoding basis, so the metadata printed below is not the same
# record five times. Each is also a different channel count.
_REQUESTED = [
    ("tangent-space normal", BakeMap.NORMAL),
    ("ambient occlusion", BakeMap.AO),
    ("object-space position", BakeMap.OBJECT_POSITION),
    ("thickness", BakeMap.THICKNESS),
    ("object id", BakeMap.OBJECT_ID),
    ("uv density", BakeMap.UV_DENSITY),
]


def describe_capabilities() -> None:
    """Step 1: what can this build produce? A consumer never hard-codes this."""
    maps = bake_provider_maps()
    print(f"  this build produces {len(maps)} maps:")
    for entry in maps:
        field = " (a field evaluator alone can produce it)" if entry.field_capable else ""
        print(f"    {entry.name:<16} {entry.channels} ch  {entry.color_space:<6} "
              f"{_BASIS_NAMES[entry.encoding_basis]}{field}")
    print(f"  with a field evaluator and no Target: {bake_provider_map_list(True)}")


def describe_result(title: str, result) -> None:
    """Everything the provider hands back beside the pixels."""
    print(f"  {title}: {result.width}x{result.height} x{result.channels}, "
          f"{_BASIS_NAMES[result.encoding.basis]}")
    print(f"      up axis {'z' if result.encoding.up_axis else 'y'}, "
          f"normal green {'+Y (OpenGL)' if result.normal_green_plus_y else '-Y (DirectX)'}, "
          f"{result.texels_covered} texels covered")
    print(f"      padding radius {result.padding.radius}, "
          f"{result.padding.texels_filled} texels filled")
    if result.encoding.basis == EncodingBasis.OBJECT_BOUNDS:
        print(f"      decode with min + v * (max - min): "
              f"min={tuple(round(v, 3) for v in result.encoding.bounds_min)} "
              f"max={tuple(round(v, 3) for v in result.encoding.bounds_max)}")
    if result.encoding.basis == EncodingBasis.DISTANCE:
        print(f"      distances were multiplied by {result.encoding.scale}")
    if result.encoding.basis == EncodingBasis.ID_COLOR:
        print(f"      ids from '{result.encoding.id_source}', "
              f"{result.id_color_count} of them; a picked colour resolves through "
              f"{[row.color for row in result.encoding.id_colors[:4]]}")


def main() -> None:
    c.require_engine()
    print("Bake provider — a consumer driving this repository's C ABI\n")
    describe_capabilities()

    with tempfile.TemporaryDirectory() as tmp:
        low_path = os.path.join(tmp, "low.obj")
        high_path = os.path.join(tmp, "high.obj")
        c.uv_sphere_obj(low_path, rings=24, segments=48, bump=0.0)
        c.uv_sphere_obj(high_path, rings=96, segments=160, bump=0.16)

        params = BakeParams(width=256, height=256, cage_distance=0.35,
                            ao_samples=32, ao_radius=0.5)
        maps = []
        with Mesh.load_obj(low_path) as low, Mesh.load_obj(high_path) as high:
            # Step 2: size the request. This validates everything and casts no
            # ray, so a consumer learns both how much memory it needs and
            # whether the request would be accepted at all, before allocating.
            sized = bake_provider_size(low, BakeMap.NORMAL, params, high=high)
            print(f"\n  sizing a {sized.width}x{sized.height} normal map: "
                  f"{sized.pixel_count} floats, no bake performed")

            # Step 3: request, into buffers the consumer owns.
            print()
            for title, kind in _REQUESTED:
                pixels, result = bake_provider_bake(low, kind, params, high=high)
                describe_result(title, result)
                maps.append((title, pixels))

            # Step 4: progress and cancellation. A host draws a bar from the
            # first and offers a button for the second; a cancelled request
            # hands back no pixels at all, not a half-finished map.
            print()
            reports = []
            bake_provider_bake(low, BakeMap.AO, params, high=high,
                               progress=lambda fraction, stage: reports.append(fraction))
            print(f"  progress reported {len(reports)} times while the AO map accumulated")

            try:
                bake_provider_bake(low, BakeMap.AO, params, high=high,
                                   cancel=lambda: True)
                print("  UNEXPECTED: a cancelled bake returned a map")
            except CyberError as exc:
                print(f"  cancelled mid-bake: {exc.message} — and no pixels came back")

            # Step 5: the refusal that matters. A map this build cannot produce
            # is NAMED, together with the advertised set. It is never quietly
            # substituted with a neutral value: a smart material silently
            # reading flat grey for curvature looks subtly wrong on a new model
            # instead of loudly broken.
            print()
            try:
                bake_provider_bake(low, 4242, params, high=high)
                print("  UNEXPECTED: an unproducible map was accepted")
            except CyberError as exc:
                print(f"  refused: {exc.message}")

        c.render_maps(
            maps, os.path.join(c.OUTPUT_DIR, "26_bake_provider.png"),
            suptitle="Bake provider — maps requested through the C ABI seam",
        )


if __name__ == "__main__":
    main()
