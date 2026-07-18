#!/usr/bin/env python3
"""Surface baking: capture a high-poly's detail into low-poly texture maps.

Bakes a bumpy high-poly sphere (the Target) onto a smooth low-poly sphere's UV
layout and renders the tangent-space normal, ambient-occlusion and displacement
maps. Ray casting dispatches through the accel layer, so with a GPU build
(``--preset linux-cuda``) the bake runs on the GPU.
"""

import os
import tempfile

import common as c
from cyberremesh import BakeMap, BakeParams, Mesh, bake


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        low_path = os.path.join(tmp, "low.obj")
        high_path = os.path.join(tmp, "high.obj")
        # Smooth low-poly (carries UVs) + a detailed, dimpled high-poly Target.
        c.uv_sphere_obj(low_path, rings=24, segments=48, bump=0.0)
        c.uv_sphere_obj(high_path, rings=96, segments=160, bump=0.16)

        # 320^2 with 16 AO rays keeps the CPU run brisk; a GPU build is far
        # faster (ray casting dispatches through the accel layer).
        params = BakeParams(width=320, height=320, cage_distance=0.35, ao_samples=16, ao_radius=0.5)

        maps = []
        with Mesh.load_obj(low_path) as low, Mesh.load_obj(high_path) as high:
            # Normal + displacement project the high-poly detail onto the smooth
            # low-poly's UVs (high -> low).
            for title, kind, png in [
                ("tangent-space normal", BakeMap.NORMAL, "07_bake_normal.png"),
                ("displacement", BakeMap.DISPLACEMENT, "07_bake_displacement.png"),
            ]:
                img = bake(low, high, kind, params)
                maps.append((title, img.to_numpy()))
                img.save_png(os.path.join(c.OUTPUT_DIR, png))
                print(f"  baked {title}: {img.width}x{img.height} x{img.channels}")
                img.close()

            # Ambient occlusion is the detailed surface's self-occlusion, baked
            # onto its own UVs (high -> high): valleys darken, peaks stay open.
            ao = bake(high, high, BakeMap.AO, params)
            maps.insert(1, ("ambient occlusion", ao.to_numpy()))
            ao.save_png(os.path.join(c.OUTPUT_DIR, "07_bake_ao.png"))
            print(f"  baked ambient occlusion: {ao.width}x{ao.height} x{ao.channels}")
            ao.close()

        c.render_maps(
            maps, os.path.join(c.OUTPUT_DIR, "07_baking.png"),
            suptitle="Surface baking — high-poly detail → low-poly maps",
        )


if __name__ == "__main__":
    main()
