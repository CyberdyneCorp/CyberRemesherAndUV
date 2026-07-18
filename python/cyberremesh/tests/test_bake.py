#!/usr/bin/env python3
"""Surface-baking binding test. Self-skips (exit 0) when the shared library is
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


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 0

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
        print("PASS bake: 16x16 tangent-space normal map, centre points up")
    finally:
        os.unlink(obj.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
