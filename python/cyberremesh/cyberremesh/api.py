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

    def _to_c(self) -> "_ffi.CyberRemeshParams":
        return _ffi.CyberRemeshParams(
            target_quad_count=int(self.target_quad_count),
            edge_scale=float(self.edge_scale),
            sharp_edge_degrees=float(self.sharp_edge_degrees),
            smooth_normal_degrees=float(self.smooth_normal_degrees),
            adaptivity=float(self.adaptivity),
            pure_quads=1 if self.pure_quads else 0,
            hole_fill_max_boundary=int(self.hole_fill_max_boundary),
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
