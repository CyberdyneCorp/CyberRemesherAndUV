"""Pythonic wrapper over the CyberRemesher C ABI.

Exposes a small, idiomatic surface — :class:`Mesh`, :class:`RemeshParams`,
:func:`remesh` — that hides handle lifetime, status-code checking and the
ctypes callback marshalling behind normal Python objects and exceptions.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from typing import Callable, Optional

from . import _ffi

# NumPy is an optional dependency: when present, meshes gain an ndarray
# ``positions`` accessor; when absent, that helper is simply not defined.
try:  # pragma: no cover - trivial import guard
    import numpy as _np

    HAVE_NUMPY = True
except ImportError:  # pragma: no cover
    _np = None
    HAVE_NUMPY = False


__all__ = [
    "CyberError",
    "Mesh",
    "RemeshParams",
    "Statistics",
    "remesh",
    "version",
    "is_available",
    "HAVE_NUMPY",
    "BakeMap",
    "BakeParams",
    "Image",
    "bake",
]


class CyberError(RuntimeError):
    """Raised when a C ABI call returns a non-OK status.

    Carries the numeric :attr:`status` and the engine's thread-local
    ``cyber_last_error()`` message.
    """

    def __init__(self, status: int, message: Optional[str] = None):
        self.status = status
        self.message = message or ""
        detail = self.message or "(no detail)"
        super().__init__(
            "{0}: {1}".format(_ffi.status_name(status), detail)
        )


def _last_error() -> str:
    raw = _ffi.get_lib().cyber_last_error()
    if not raw:
        return ""
    return raw.decode("utf-8", "replace")


def _check(status: int) -> None:
    if status != _ffi.STATUS_OK:
        raise CyberError(status, _last_error())


def version() -> str:
    """Return the engine runtime version string (major.minor.patch)."""
    major, minor, patch = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
    _ffi.get_lib().cyber_version(
        ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch)
    )
    return f"{major.value}.{minor.value}.{patch.value}"


def is_available() -> bool:
    """True if the underlying shared library can be loaded."""
    return _ffi.is_available()


@dataclass
class Statistics:
    """Result counters from a remesh run (mirror of ``cyber_statistics``)."""

    vertices: int = 0
    quads: int = 0
    triangles: int = 0
    other_polygons: int = 0
    islands: int = 0
    islands_failed: int = 0

    @classmethod
    def _from_c(cls, c: "_ffi.CyberStatistics") -> "Statistics":
        return cls(
            vertices=int(c.vertex_count),
            quads=int(c.quad_count),
            triangles=int(c.triangle_count),
            other_polygons=int(c.other_polygon_count),
            islands=int(c.island_count),
            islands_failed=int(c.islands_failed),
        )


@dataclass
class RemeshParams:
    """Canonical user-facing remeshing parameters.

    Defaults and valid ranges match ``cyber::remesh::Parameters`` (the single
    source of truth in the C++ core). Out-of-range values are clamped by the
    engine, not here.
    """

    target_quad_count: int = 50_000  # 100 .. 2_000_000
    edge_scale: float = 1.0  # 0.5 .. 4.0
    sharp_edge_degrees: float = 90.0  # 30.0 .. 180.0
    smooth_normal_degrees: float = 0.0  # 0.0 .. 180.0
    adaptivity: float = 1.0  # 0.0 .. 1.0
    pure_quads: bool = False
    hole_fill_max_boundary: int = 64  # 0 (never) .. 10_000
    # Quadrangulator: "field-aligned" (default, max-matching, highest
    # dominance), "instant-meshes" (position-field extractor, more uniform
    # field-aligned flow with fewer/better singularities), "integer" (the
    # integer-parametrization extractor, Milestones 3-5 — watertight/manifold,
    # experimental; degrades at coarse target counts), or "quad-cover"
    # (QuadCover seamless-UV isoline extractor — ~1% irregular on closed
    # surfaces, but requires the CYBER_QUADCOVER_CLI environment variable to
    # point at a built autoremesher_cli; without it the run fails cleanly).
    quad_method: str = "quad-cover"

    _QUAD_METHODS = {
        "field-aligned": 0,
        "instant-meshes": 1,
        "integer": 2,
        "quad-cover": 3,
    }

    def _to_c(self) -> "_ffi.CyberRemeshParams":
        try:
            method = self._QUAD_METHODS[self.quad_method]
        except KeyError:
            raise ValueError(
                f"quad_method must be one of {sorted(self._QUAD_METHODS)}, got {self.quad_method!r}"
            ) from None
        return _ffi.CyberRemeshParams(
            target_quad_count=int(self.target_quad_count),
            edge_scale=float(self.edge_scale),
            sharp_edge_degrees=float(self.sharp_edge_degrees),
            smooth_normal_degrees=float(self.smooth_normal_degrees),
            adaptivity=float(self.adaptivity),
            pure_quads=1 if self.pure_quads else 0,
            hole_fill_max_boundary=int(self.hole_fill_max_boundary),
            quad_method=method,
        )


@dataclass
class AtlasParams:
    """Automatic UV-atlas parameters (mirror of ``cyber::uv::AtlasOptions``)."""

    # A face joins a growing chart while its normal stays within this angle of
    # the chart's seed normal. Smaller => more, flatter charts (less angular
    # distortion, more seams).
    max_chart_angle_degrees: float = 40.0
    pack_margin: float = 0.0  # gap around each island, in UV units
    texture_size: int = 1024  # resolution for the texel-density readout

    def _to_c(self) -> "_ffi.CyberAtlasParams":
        return _ffi.CyberAtlasParams(
            max_chart_angle_degrees=float(self.max_chart_angle_degrees),
            pack_margin=float(self.pack_margin),
            texture_size=int(self.texture_size),
        )


@dataclass
class AtlasResult:
    """Aggregate atlas quality/packing report (mirror of ``CyberAtlasResult``)."""

    chart_count: int = 0
    seam_edges: int = 0
    max_angle_distortion: float = 0.0
    rms_angle_distortion: float = 0.0
    flipped_charts: int = 0
    fallback_charts: int = 0
    packed_area: float = 0.0
    texel_density: float = 0.0

    @classmethod
    def _from_c(cls, c: "_ffi.CyberAtlasResult") -> "AtlasResult":
        return cls(
            chart_count=int(c.chart_count),
            seam_edges=int(c.seam_edges),
            max_angle_distortion=float(c.max_angle_distortion),
            rms_angle_distortion=float(c.rms_angle_distortion),
            flipped_charts=int(c.flipped_charts),
            fallback_charts=int(c.fallback_charts),
            packed_area=float(c.packed_area),
            texel_density=float(c.texel_density),
        )


class Mesh:
    """A handle to an engine mesh.

    Construct an empty mesh, load one from disk with :meth:`load_obj`, or
    receive one from :func:`remesh`. The underlying handle is released on
    :meth:`close` or garbage collection.
    """

    __slots__ = ("_handle", "_stats")

    def __init__(self, handle: Optional[int] = None):
        if handle is None:
            handle = _ffi.get_lib().cyber_mesh_create()
            if not handle:
                raise CyberError(_ffi.STATUS_ERROR, _last_error())
        self._handle = handle
        self._stats: Optional[Statistics] = None

    # -- lifetime -----------------------------------------------------------
    @property
    def handle(self) -> int:
        if self._handle is None:
            raise ValueError("operation on a closed Mesh")
        return self._handle

    def close(self) -> None:
        """Release the underlying engine handle (idempotent)."""
        if self._handle is not None:
            _ffi.get_lib().cyber_mesh_destroy(self._handle)
            self._handle = None

    def __del__(self):  # pragma: no cover - GC timing dependent
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Mesh":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # -- I/O ----------------------------------------------------------------
    @classmethod
    def load_obj(cls, path: str) -> "Mesh":
        """Load a mesh from a Wavefront ``.obj`` file."""
        out = ctypes.c_void_p()
        status = _ffi.get_lib().cyber_mesh_load_obj(
            str(path).encode("utf-8"), ctypes.byref(out)
        )
        if status != _ffi.STATUS_OK:
            raise CyberError(status, _last_error())
        return cls(handle=out.value)

    def save_obj(self, path: str) -> None:
        """Write this mesh to a Wavefront ``.obj`` file."""
        _check(
            _ffi.get_lib().cyber_mesh_save_obj(
                self.handle, str(path).encode("utf-8")
            )
        )

    def unwrap_atlas(self, params: Optional["AtlasParams"] = None) -> "AtlasResult":
        """Generate an automatic UV atlas for this mesh, IN PLACE.

        Seams the mesh into normal-coherent charts, LSCM-unwraps each, packs
        them into the unit square and writes the per-corner ``uv`` attribute, so
        a subsequent :meth:`save_obj` emits ``vt`` / ``f v/vt``. Returns an
        :class:`AtlasResult` with distortion and packing statistics.
        """
        if params is None:
            params = AtlasParams()
        c_params = params._to_c()
        c_result = _ffi.CyberAtlasResult()
        _check(
            _ffi.get_lib().cyber_uv_atlas(
                self.handle, ctypes.byref(c_params), ctypes.byref(c_result)
            )
        )
        return AtlasResult._from_c(c_result)

    # -- queries ------------------------------------------------------------
    @property
    def vertex_count(self) -> int:
        return int(_ffi.get_lib().cyber_mesh_vertex_count(self.handle))

    @property
    def face_count(self) -> int:
        return int(_ffi.get_lib().cyber_mesh_face_count(self.handle))

    @property
    def stats(self) -> Optional[Statistics]:
        """Statistics from the run that produced this mesh, if any."""
        return self._stats

    def _copy_positions(self) -> "ctypes.Array":
        lib = _ffi.get_lib()
        needed = int(lib.cyber_mesh_copy_positions(self.handle, None, 0))
        buf = (ctypes.c_float * needed)()
        if needed:
            lib.cyber_mesh_copy_positions(self.handle, buf, needed)
        return buf

    if HAVE_NUMPY:

        @property
        def positions(self):
            """Vertex positions as an ``(n, 3)`` float32 ndarray (a snapshot).

            The engine stores vertices in an index-addressed pool, so this is a
            packed copy rather than a live view into engine memory.
            """
            buf = self._copy_positions()
            arr = _np.frombuffer(buf, dtype=_np.float32).copy()
            return arr.reshape((-1, 3))


def remesh(
    mesh: Mesh,
    params: Optional[RemeshParams] = None,
    progress: Optional[Callable[[float, str], None]] = None,
    cancel: Optional[Callable[[], bool]] = None,
) -> Mesh:
    """Run the automatic quad-remeshing pipeline on ``mesh``.

    ``mesh`` is never modified; a new :class:`Mesh` is returned with its
    :attr:`Mesh.stats` populated.

    ``progress`` — optional ``callable(fraction: float, stage: str)`` invoked
    as the pipeline advances. ``cancel`` — optional ``callable() -> bool``
    polled cooperatively; return ``True`` to abort (raises
    :class:`CyberError` with a ``CANCELLED`` status).

    Exceptions raised inside the Python callbacks are swallowed at the C
    boundary (they must never unwind through C) but the pipeline continues; a
    raising ``cancel`` is treated as "do not cancel".
    """
    if params is None:
        params = RemeshParams()

    lib = _ffi.get_lib()

    # Keep the trampolines alive for the whole synchronous call. Even when the
    # caller passes nothing we install harmless no-op callbacks so the ABI
    # always receives valid function pointers.
    def _progress_trampoline(fraction, stage_ptr, _user):
        if progress is None:
            return
        try:
            stage = stage_ptr.decode("utf-8", "replace") if stage_ptr else ""
            progress(float(fraction), stage)
        except Exception:
            # Never let a Python exception cross back into C.
            pass

    def _cancel_trampoline(_user):
        if cancel is None:
            return 0
        try:
            return 1 if cancel() else 0
        except Exception:
            return 0

    progress_cb = _ffi.PROGRESS_CB(_progress_trampoline)
    cancel_cb = _ffi.CANCEL_CB(_cancel_trampoline)

    c_params = params._to_c()
    out_handle = ctypes.c_void_p()

    status = lib.cyber_remesh(
        mesh.handle,
        ctypes.byref(c_params),
        progress_cb,
        cancel_cb,
        None,
        ctypes.byref(out_handle),
    )
    _check(status)

    if not out_handle.value:
        raise CyberError(_ffi.STATUS_ERROR, _last_error() or "remesh produced no mesh")

    result = Mesh(handle=out_handle.value)
    # Statistics are fetched from the result mesh (the C ABI has no out-stats).
    c_stats = _ffi.CyberStatistics()
    if lib.cyber_mesh_stats(result.handle, ctypes.byref(c_stats)) == _ffi.STATUS_OK:
        result._stats = Statistics._from_c(c_stats)
    return result


# ---------------------------------------------------------------------------
# Surface baking
# ---------------------------------------------------------------------------
class BakeMap:
    """Bakeable map types (mirror of ``CyberBakeMap``)."""

    NORMAL = _ffi.BAKE_NORMAL
    AO = _ffi.BAKE_AO
    DISPLACEMENT = _ffi.BAKE_DISPLACEMENT
    POSITION = _ffi.BAKE_POSITION
    COLOR = _ffi.BAKE_COLOR


@dataclass
class BakeParams:
    """Bake settings (mirror of ``CyberBakeParams``)."""

    width: int = 512
    height: int = 512
    cage_distance: float = 0.1
    ao_samples: int = 16
    ao_radius: float = 1.0

    def _to_c(self) -> "_ffi.CyberBakeParams":
        return _ffi.CyberBakeParams(
            width=int(self.width),
            height=int(self.height),
            cage_distance=float(self.cage_distance),
            ao_samples=int(self.ao_samples),
            ao_radius=float(self.ao_radius),
        )


class Image:
    """A baked map: row-major float pixels with ``channels`` per texel.

    Read it with :meth:`to_numpy` (an ``(h, w, channels)`` array) or write it
    straight to PNG with :meth:`save_png`. Released on :meth:`close` / GC.
    """

    __slots__ = ("_handle",)

    def __init__(self, handle: int):
        self._handle = handle

    @property
    def handle(self) -> int:
        if self._handle is None:
            raise ValueError("operation on a closed Image")
        return self._handle

    @property
    def width(self) -> int:
        return int(_ffi.get_lib().cyber_image_width(self.handle))

    @property
    def height(self) -> int:
        return int(_ffi.get_lib().cyber_image_height(self.handle))

    @property
    def channels(self) -> int:
        return int(_ffi.get_lib().cyber_image_channels(self.handle))

    def save_png(self, path: str) -> None:
        """Write the map to an 8-bit PNG (tonemapped)."""
        _check(_ffi.get_lib().cyber_image_save_png(self.handle, str(path).encode("utf-8")))

    def to_numpy(self):
        """Return the map as an ``(height, width, channels)`` float32 ndarray."""
        if not HAVE_NUMPY:
            raise RuntimeError("numpy is required for Image.to_numpy()")
        lib = _ffi.get_lib()
        n = int(lib.cyber_image_copy_pixels(self.handle, None, 0))
        buf = (ctypes.c_float * n)()
        lib.cyber_image_copy_pixels(self.handle, buf, n)
        arr = _np.frombuffer(buf, dtype=_np.float32).copy()
        return arr.reshape((self.height, self.width, self.channels))

    def close(self) -> None:
        if self._handle is not None:
            _ffi.get_lib().cyber_image_free(self._handle)
            self._handle = None

    def __del__(self):  # pragma: no cover - GC timing dependent
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Image":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def bake(low: "Mesh", high: "Mesh", bake_map: int = BakeMap.NORMAL,
         params: Optional[BakeParams] = None) -> Image:
    """Bake ``bake_map`` from ``high`` (the Target) onto ``low``'s UV layout.

    ``low`` must carry UVs (load an OBJ with ``vt`` coordinates). Returns an
    :class:`Image`; a ``CyberError`` is raised on failure.
    """
    if params is None:
        params = BakeParams()
    lib = _ffi.get_lib()
    c_params = params._to_c()
    out = ctypes.c_void_p()
    status = lib.cyber_bake(
        low.handle, high.handle, int(bake_map), ctypes.byref(c_params), ctypes.byref(out)
    )
    _check(status)
    if not out.value:
        raise CyberError(_ffi.STATUS_ERROR, _last_error() or "bake produced no image")
    return Image(out.value)
