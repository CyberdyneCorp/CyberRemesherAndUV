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
    finally:
        os.unlink(obj.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
