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

        _gate_a_raising_evaluator_raises(obj.name)
        _gate_the_openness_rename_shim(obj.name)
    finally:
        os.unlink(obj.name)
    return 0


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
