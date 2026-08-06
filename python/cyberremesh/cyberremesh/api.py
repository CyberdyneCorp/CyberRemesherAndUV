"""Pythonic wrapper over the CyberRemesher C ABI.

Exposes a small, idiomatic surface — :class:`Mesh`, :class:`RemeshParams`,
:func:`remesh` — that hides handle lifetime, status-code checking and the
ctypes callback marshalling behind normal Python objects and exceptions.
"""

from __future__ import annotations

import ctypes
import warnings
from dataclasses import dataclass
from typing import Callable, List, Optional, Sequence, Tuple

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
    "Falloff",
    "Snapper",
    "SoftTransformReport",
    "SeamCostParams",
    "SeamSet",
    "SeamPath",
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
    # Quadrangulator: "quad-cover" (default), "field-aligned" (max-matching,
    # highest dominance), "instant-meshes" (position-field extractor, more
    # uniform field-aligned flow with fewer/better singularities), or "integer"
    # (the integer-parametrization extractor, Milestones 3-5 — watertight/
    # manifold, experimental; degrades at coarse target counts).
    #
    # "quad-cover" (QuadCover seamless-UV isoline extractor) always works: a
    # dependency-free native seamless-UV solver is compiled in unconditionally
    # (~4% irregular, median angle a few degrees below QuadriFlow). Building the
    # engine with -DCYBER_WITH_QUADCOVER=ON links a vendored in-process Geogram
    # solver that beats QuadriFlow on median angle *and* irregular-vertex count
    # on 3 of the 5 corpus models (spot, rocker-arm, stanford-bunny), losing
    # fandisk and cheburashka; the optional CYBER_QUADCOVER_CLI is
    # only a faster external reference path. (Earlier docs claimed the CLI was
    # required and the run "fails cleanly" without it — that was never true for
    # the shipped native solver.)
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
    # Merge adjacent charts sharing a normal cone (fewer seams, same flatness).
    merge_charts: bool = True
    # Looser second merge pass: keep merging while the union's max conformal
    # error stays <= this cap (0 disables). Spends distortion headroom for fewer
    # seams. Only used when ``merge_charts`` is True.
    max_chart_distortion: float = 0.10
    # Rotate each chart to its minimum-area bounding box before packing (tighter
    # pack / higher texel density).
    reorient_charts: bool = True

    def _to_c(self) -> "_ffi.CyberAtlasParams":
        return _ffi.CyberAtlasParams(
            max_chart_angle_degrees=float(self.max_chart_angle_degrees),
            pack_margin=float(self.pack_margin),
            texture_size=int(self.texture_size),
            reorient_charts=1 if self.reorient_charts else 0,
            merge_charts=1 if self.merge_charts else 0,
            max_chart_distortion=float(self.max_chart_distortion),
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


class Falloff:
    """Soft-selection falloff curves (mirrors ``CyberFalloff``)."""

    LINEAR = _ffi.FALLOFF_LINEAR
    SMOOTH = _ffi.FALLOFF_SMOOTH
    SHARP = _ffi.FALLOFF_SHARP
    ROUND = _ffi.FALLOFF_ROUND


@dataclass(frozen=True)
class SoftTransformReport:
    """Outcome of a weighted transform/relax."""

    moved: int
    resnapped: int
    max_snap_distance: float

    @classmethod
    def _from_c(cls, c: "_ffi.CyberSoftTransformReport") -> "SoftTransformReport":
        return cls(
            moved=int(c.moved),
            resnapped=int(c.resnapped),
            max_snap_distance=float(c.max_snap_distance),
        )


class Snapper:
    """Snapshot snapper over a Target mesh (a BVH for closest-surface queries).

    Snapshot semantics: it does not observe later changes to the Target, so
    rebuild it whenever the Target changes. Usable as a context manager.
    """

    __slots__ = ("_handle",)

    def __init__(self, target: "Mesh"):
        out = ctypes.c_void_p()
        status = _ffi.get_lib().cyber_snapper_create(target.handle, ctypes.byref(out))
        if status != _ffi.STATUS_OK:
            raise CyberError(status, _last_error())
        self._handle = out.value

    @property
    def handle(self) -> int:
        if self._handle is None:
            raise ValueError("operation on a closed Snapper")
        return self._handle

    def close(self) -> None:
        """Release the underlying engine handle (idempotent)."""
        if self._handle is not None:
            _ffi.get_lib().cyber_snapper_free(self._handle)
            self._handle = None

    def __del__(self):  # pragma: no cover - GC timing dependent
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Snapper":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def _vec3(v: Sequence[float]) -> "ctypes.Array":
    return (ctypes.c_float * 3)(float(v[0]), float(v[1]), float(v[2]))


def _read_ids(reader: Callable[[Optional["ctypes.Array"], int], int]) -> List[int]:
    """copy_positions convention: size query, then fill."""
    total = reader(None, 0)
    if total == 0:
        return []
    buf = (ctypes.c_uint32 * total)()
    filled = reader(buf, total)
    return [int(buf[i]) for i in range(min(filled, total))]


@dataclass
class SeamCostParams:
    """Seam-routing cost multipliers (mirror of ``cyber::uv::SeamCostOptions``).

    Each field is a multiplier on an edge's length; lower means "prefer this
    edge". Defaults bias the route toward feature-tagged and concave (valley)
    edges so a routed seam follows the groove instead of the flat shortcut.
    """

    flat_weight: float = 1.0
    feature_weight: float = 0.25
    concave_weight: float = 0.35
    crease_degrees: float = 20.0
    min_weight: float = 1e-3

    def _to_c(self) -> "_ffi.CyberSeamPathOptions":
        return _ffi.CyberSeamPathOptions(
            flat_weight=float(self.flat_weight),
            feature_weight=float(self.feature_weight),
            concave_weight=float(self.concave_weight),
            crease_degrees=float(self.crease_degrees),
            min_weight=float(self.min_weight),
        )

    @classmethod
    def defaults(cls) -> "SeamCostParams":
        """The engine's own defaults, read back through the ABI."""
        c = _ffi.CyberSeamPathOptions()
        _ffi.get_lib().cyber_default_seam_path_options(ctypes.byref(c))
        return cls(
            flat_weight=float(c.flat_weight),
            feature_weight=float(c.feature_weight),
            concave_weight=float(c.concave_weight),
            crease_degrees=float(c.crease_degrees),
            min_weight=float(c.min_weight),
        )


class _Handle:
    """Shared close/__del__/context-manager plumbing for opaque ABI handles."""

    __slots__ = ("_handle",)
    _free_symbol = ""

    @property
    def handle(self) -> int:
        if self._handle is None:
            raise ValueError("operation on a closed {0}".format(type(self).__name__))
        return self._handle

    def close(self) -> None:
        """Release the underlying engine handle (idempotent)."""
        if self._handle is not None:
            getattr(_ffi.get_lib(), self._free_symbol)(self._handle)
            self._handle = None

    def __del__(self):  # pragma: no cover - GC timing dependent
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class SeamSet(_Handle):
    """A mutable set of seam edges — the model gesture unwrap and sew read.

    Routed seam paths commit into it exactly like hand-drawn Pencil strokes.
    """

    __slots__ = ()
    _free_symbol = "cyber_seam_set_free"

    def __init__(self):
        out = ctypes.c_void_p()
        _check(_ffi.get_lib().cyber_seam_set_create(ctypes.byref(out)))
        self._handle = out.value

    def __len__(self) -> int:
        return int(_ffi.get_lib().cyber_seam_set_count(self.handle))

    def is_seam(self, edge: int) -> bool:
        return _ffi.get_lib().cyber_seam_set_is_seam(self.handle, int(edge)) == 1

    def edges(self) -> List[int]:
        """Seam edge ids in ascending order."""
        lib = _ffi.get_lib()
        return _read_ids(lambda buf, n: lib.cyber_seam_set_edges(self.handle, buf, n))

    def mark(self, edge: int) -> None:
        _check(_ffi.get_lib().cyber_seam_set_mark(self.handle, int(edge)))

    def erase(self, edge: int) -> None:
        _check(_ffi.get_lib().cyber_seam_set_erase(self.handle, int(edge)))

    def revert_commit(self, marked_edges: Sequence[int]) -> None:
        """Undo a :meth:`SeamPath.commit` from the edge ids it returned."""
        for edge in marked_edges:
            self.erase(edge)


class SeamPath(_Handle):
    """An editable, auto-routed seam path (the UV Path tool).

    Place waypoints with :meth:`add_waypoint`; the engine routes least-cost
    edge paths between consecutive ones, biased toward feature and valley
    edges. Waypoints stay editable until :meth:`commit`, which marks the route
    into a :class:`SeamSet` and arms :attr:`resume_marker`.

    Snapshot semantics, as for :class:`Snapper`: the path references the mesh
    it was built on, so recreate it if that mesh is edited.
    """

    __slots__ = ()
    _free_symbol = "cyber_seam_path_free"

    def __init__(self, mesh: "Mesh", params: Optional[SeamCostParams] = None):
        c_params = (params or SeamCostParams())._to_c()
        out = ctypes.c_void_p()
        _check(
            _ffi.get_lib().cyber_seam_path_create(
                mesh.handle, ctypes.byref(c_params), ctypes.byref(out)
            )
        )
        self._handle = out.value

    # -- editing ------------------------------------------------------------
    def add_waypoint(self, vertex: int) -> bool:
        return _ffi.get_lib().cyber_seam_path_add_waypoint(self.handle, int(vertex)) == 1

    def move_waypoint(self, index: int, vertex: int) -> bool:
        return (
            _ffi.get_lib().cyber_seam_path_move_waypoint(self.handle, int(index), int(vertex))
            == 1
        )

    def move_waypoint_to(self, index: int, position: Sequence[float], radius: float) -> bool:
        """Drag waypoint ``index`` onto the nearest vertex within ``radius``."""
        return (
            _ffi.get_lib().cyber_seam_path_move_waypoint_to(
                self.handle, int(index), _vec3(position), float(radius)
            )
            == 1
        )

    def remove_waypoint(self, index: int) -> bool:
        return _ffi.get_lib().cyber_seam_path_remove_waypoint(self.handle, int(index)) == 1

    def clear(self) -> None:
        """Drop the pending waypoints; committed seams and the marker stay."""
        _ffi.get_lib().cyber_seam_path_clear(self.handle)

    # -- pending state ------------------------------------------------------
    def waypoint_count(self) -> int:
        return int(_ffi.get_lib().cyber_seam_path_waypoint_count(self.handle))

    def waypoints(self) -> List[int]:
        lib = _ffi.get_lib()
        return _read_ids(lambda buf, n: lib.cyber_seam_path_waypoints(self.handle, buf, n))

    def segment_count(self) -> int:
        return int(_ffi.get_lib().cyber_seam_path_segment_count(self.handle))

    def segment(self, index: int) -> List[int]:
        lib = _ffi.get_lib()
        return _read_ids(
            lambda buf, n: lib.cyber_seam_path_segment(self.handle, int(index), buf, n)
        )

    def segment_revision(self, index: int) -> int:
        """Counter bumped each time THIS segment re-routes."""
        return int(_ffi.get_lib().cyber_seam_path_segment_revision(self.handle, int(index)))

    def is_routed(self) -> bool:
        return _ffi.get_lib().cyber_seam_path_is_routed(self.handle) == 1

    def vertices(self) -> List[int]:
        lib = _ffi.get_lib()
        return _read_ids(lambda buf, n: lib.cyber_seam_path_vertices(self.handle, buf, n))

    def edges(self) -> List[int]:
        lib = _ffi.get_lib()
        return _read_ids(lambda buf, n: lib.cyber_seam_path_edges(self.handle, buf, n))

    # -- commit / resume ----------------------------------------------------
    def commit(self, seams: SeamSet) -> List[int]:
        """Turn the routed path into seams; returns the newly marked edge ids.

        That list is the undo record: erasing exactly those ids restores the
        pre-commit state (edges that were already seams are never listed).
        """
        # commit() MUTATES, so the usual size-query-then-fill dance would
        # commit twice. The pending edge count is an exact upper bound on the
        # newly marked ones, so size the buffer from it and call once.
        capacity = len(self.edges())
        buf = (ctypes.c_uint32 * capacity)() if capacity else None
        total = _ffi.get_lib().cyber_seam_path_commit(
            self.handle, seams.handle, buf, capacity
        )
        return [int(buf[i]) for i in range(min(int(total), capacity))]

    @property
    def resume_marker(self) -> Optional[int]:
        """Vertex the last commit ended on, or None when no marker is armed."""
        value = int(_ffi.get_lib().cyber_seam_path_resume_marker(self.handle))
        return None if value == _ffi.INVALID_ID else value

    def drop_resume_marker(self) -> None:
        """Start the next path fresh. Never touches a committed seam."""
        _ffi.get_lib().cyber_seam_path_drop_resume_marker(self.handle)


class Mesh:
    """A handle to an engine mesh.

    Construct an empty mesh, load one from disk with :meth:`load_obj`, or
    receive one from :func:`remesh`. The underlying handle is released on
    :meth:`close` or garbage collection.
    """

    __slots__ = ("_handle", "_stats", "guidance_warnings")

    def __init__(self, handle: Optional[int] = None):
        if handle is None:
            handle = _ffi.get_lib().cyber_mesh_create()
            if not handle:
                raise CyberError(_ffi.STATUS_ERROR, _last_error())
        self._handle = handle
        self._stats: Optional[Statistics] = None
        # Guidance the engine could not honor (see remesh()). Always present so
        # callers can read it unconditionally.
        self.guidance_warnings: List[str] = []

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

    def edge_signed_dihedral(self, edge: int) -> float:
        """Dihedral angle of an edge in degrees: >0 in a valley, <0 on a ridge.

        0 for flat edges and for edges that are not two-face manifold. This is
        the curvature term :class:`SeamPath` routes on.
        """
        return float(
            _ffi.get_lib().cyber_mesh_edge_signed_dihedral(self.handle, int(edge))
        )

    # -- soft selection -----------------------------------------------------
    #
    # The weight field lives on this handle. Region ops replace it (line,
    # sphere) or accumulate into it (paint); the selection ops reshape it; the
    # weighted transform/relax consume it. Passing a :class:`Snapper` to
    # :meth:`transform_selection` / :meth:`relax_selection` glues the moved
    # vertices to the Target INSIDE the same call — there is no separate snap
    # pass, and running one afterwards would drag the untouched vertices too.
    # Vertices at weight 0 are never moved and never re-snapped.

    def select_line(
        self,
        anchor: Sequence[float],
        end: Sequence[float],
        view_dir: Sequence[float] = (0.0, 0.0, 1.0),
        snap_angle: bool = False,
        snap_degrees: float = 15.0,
        falloff: int = Falloff.SMOOTH,
    ) -> None:
        """Line gradient: 0 at ``anchor``, 1 at ``end`` and 1 beyond it."""
        _check(
            _ffi.get_lib().cyber_retopo_selection_line(
                self.handle, _vec3(anchor), _vec3(end), _vec3(view_dir),
                1 if snap_angle else 0, float(snap_degrees), int(falloff),
            )
        )

    def select_sphere(
        self, center: Sequence[float], radius: float, falloff: int = Falloff.SMOOTH
    ) -> None:
        """Sphere region: 1 at ``center`` falling to 0 at ``radius``."""
        _check(
            _ffi.get_lib().cyber_retopo_selection_sphere(
                self.handle, _vec3(center), float(radius), int(falloff)
            )
        )

    def paint_selection(
        self,
        center: Sequence[float],
        radius: float,
        pressure: float = 1.0,
        subtract: bool = False,
        falloff: int = Falloff.SMOOTH,
    ) -> None:
        """Accumulate one brush dab into the selection."""
        _check(
            _ffi.get_lib().cyber_retopo_selection_paint(
                self.handle, _vec3(center), float(radius), float(pressure),
                1 if subtract else 0, int(falloff),
            )
        )

    def paint_selection_stroke(
        self,
        samples: Sequence[Tuple[float, float, float, float]],
        radius: float,
        subtract: bool = False,
        falloff: int = Falloff.SMOOTH,
    ) -> None:
        """Accumulate a whole gesture: ``(x, y, z, pressure)`` samples."""
        flat = [float(v) for sample in samples for v in sample]
        buf = (ctypes.c_float * len(flat))(*flat)
        _check(
            _ffi.get_lib().cyber_retopo_selection_paint_stroke(
                self.handle, buf, len(samples), float(radius),
                1 if subtract else 0, int(falloff),
            )
        )

    def clear_selection(self) -> None:
        """Zero every weight."""
        _check(_ffi.get_lib().cyber_retopo_selection_clear(self.handle))

    def invert_selection(self) -> None:
        """``w -> 1 - w`` over the live vertices."""
        _check(_ffi.get_lib().cyber_retopo_selection_invert(self.handle))

    def expand_selection(self, steps: int = 1) -> None:
        """Grow the selection by the one-ring maximum, ``steps`` times."""
        _check(_ffi.get_lib().cyber_retopo_selection_expand(self.handle, int(steps)))

    def contract_selection(self, steps: int = 1) -> None:
        """Shrink the selection by the one-ring minimum, ``steps`` times."""
        _check(_ffi.get_lib().cyber_retopo_selection_contract(self.handle, int(steps)))

    def smooth_selection(self, steps: int = 1) -> None:
        """Soften the weight transition (the shell's smooth by 1 / 5 / 10)."""
        _check(_ffi.get_lib().cyber_retopo_selection_smooth(self.handle, int(steps)))

    def selection_weights(self) -> List[float]:
        """The weight field as a list indexed by vertex id (a snapshot)."""
        lib = _ffi.get_lib()
        needed = int(lib.cyber_retopo_selection_copy_weights(self.handle, None, 0))
        buf = (ctypes.c_float * needed)()
        if needed:
            lib.cyber_retopo_selection_copy_weights(self.handle, buf, needed)
        return list(buf)

    def set_selection_weights(self, weights: Sequence[float]) -> None:
        """Replace the whole weight field (values are clamped into [0, 1])."""
        buf = (ctypes.c_float * len(weights))(*[float(w) for w in weights])
        _check(
            _ffi.get_lib().cyber_retopo_selection_set_weights(
                self.handle, buf, len(weights)
            )
        )

    def save_selection(self, name: str) -> None:
        """Store the current selection in the named slot on this handle."""
        _check(
            _ffi.get_lib().cyber_retopo_selection_save(
                self.handle, str(name).encode("utf-8")
            )
        )

    def load_selection(self, name: str) -> None:
        """Restore the selection from the named slot."""
        _check(
            _ffi.get_lib().cyber_retopo_selection_load(
                self.handle, str(name).encode("utf-8")
            )
        )

    def selection_slots(self) -> List[str]:
        """Names of the saved selection slots, in name order."""
        lib = _ffi.get_lib()
        count = int(lib.cyber_retopo_selection_slot_count(self.handle))
        names = []
        for i in range(count):
            raw = lib.cyber_retopo_selection_slot_name(self.handle, i)
            names.append(raw.decode("utf-8") if raw else "")
        return names

    def transform_selection(
        self,
        transform: Sequence[float],
        snapper: Optional["Snapper"] = None,
        resnap_epsilon: float = 0.0,
    ) -> SoftTransformReport:
        """Apply a 12-float affine per vertex scaled by its selection weight.

        ``transform`` is the column-major 3x3 linear part followed by the
        translation, matching ``cyber_retopo_transform_vertices``.
        """
        buf = (ctypes.c_float * 12)(*[float(v) for v in transform])
        report = _ffi.CyberSoftTransformReport()
        _check(
            _ffi.get_lib().cyber_retopo_selection_transform(
                self.handle, buf, snapper.handle if snapper else None,
                float(resnap_epsilon), ctypes.byref(report),
            )
        )
        return SoftTransformReport._from_c(report)

    def relax_selection(
        self,
        strength: float = 0.5,
        iterations: int = 1,
        pinned: Optional[Sequence[int]] = None,
        snapper: Optional["Snapper"] = None,
        resnap_epsilon: float = 0.0,
    ) -> SoftTransformReport:
        """Relax the mesh with the per-vertex blend scaled by the weights."""
        pins = pinned or []
        pin_buf = (ctypes.c_uint32 * len(pins))(*[int(p) for p in pins]) if pins else None
        report = _ffi.CyberSoftTransformReport()
        _check(
            _ffi.get_lib().cyber_retopo_selection_relax(
                self.handle, float(strength), int(iterations), pin_buf, len(pins),
                snapper.handle if snapper else None, float(resnap_epsilon),
                ctypes.byref(report),
            )
        )
        return SoftTransformReport._from_c(report)

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


@dataclass
class FlowGuide:
    """A user-drawn flow guide: an ordered polyline on or near the surface.

    The cross field is softly biased toward the guide's tangent within
    ``radius`` of the stroke, scaled by ``strength`` (clamped to ``[0, 1]``).
    ``radius`` is a world-space distance and must be greater than 0 — a
    zero-radius guide could never be honored, so it is rejected rather than
    silently ignored.
    """

    points: Sequence[Sequence[float]]
    strength: float = 1.0
    radius: float = 0.0


def remesh(
    mesh: Mesh,
    params: Optional[RemeshParams] = None,
    progress: Optional[Callable[[float, str], None]] = None,
    cancel: Optional[Callable[[], bool]] = None,
    guides: Optional[Sequence[FlowGuide]] = None,
    density: Optional[Sequence[float]] = None,
    density_per_face: bool = False,
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

    ``guides`` — optional :class:`FlowGuide` strokes biasing the cross field.
    ``density`` — optional painted sizing multiplier, one value per input
    vertex (or per face with ``density_per_face=True``); values are clamped to
    ``[0.25, 4.0]`` and the local edge length becomes ``base / sqrt(density)``.

    Guidance the engine could not honor is never dropped silently: the
    messages are attached to the result as ``Mesh.guidance_warnings`` AND
    re-raised through :func:`warnings.warn`, so a script that ignores the
    attribute still sees them.
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

    guidance_warnings: List[str] = []

    def _warning_trampoline(message_ptr, _user):
        try:
            if message_ptr:
                guidance_warnings.append(message_ptr.decode("utf-8", "replace"))
        except Exception:
            pass

    progress_cb = _ffi.PROGRESS_CB(_progress_trampoline)
    cancel_cb = _ffi.CANCEL_CB(_cancel_trampoline)
    warning_cb = _ffi.WARNING_CB(_warning_trampoline)

    c_params = params._to_c()
    out_handle = ctypes.c_void_p()

    if guides is None and density is None:
        status = lib.cyber_remesh(
            mesh.handle,
            ctypes.byref(c_params),
            progress_cb,
            cancel_cb,
            None,
            ctypes.byref(out_handle),
        )
    else:
        # Every buffer below must stay alive across the call, so the point
        # arrays are held in `keepalive` rather than built inline.
        keepalive: List[object] = []
        c_guides = (_ffi.CyberFlowGuide * len(guides or []))()
        for i, guide in enumerate(guides or []):
            flat = [float(c) for point in guide.points for c in point]
            buf = (ctypes.c_float * len(flat))(*flat)
            keepalive.append(buf)
            c_guides[i].points = ctypes.cast(buf, ctypes.POINTER(ctypes.c_float))
            c_guides[i].point_count = len(guide.points)
            c_guides[i].strength = float(guide.strength)
            c_guides[i].radius = float(guide.radius)
        c_guidance = _ffi.CyberGuidance()
        c_guidance.guides = (
            ctypes.cast(c_guides, ctypes.POINTER(_ffi.CyberFlowGuide)) if guides else None
        )
        c_guidance.guide_count = len(guides or [])
        density_buf = None
        if density is not None:
            values = [float(v) for v in density]
            density_buf = (ctypes.c_float * len(values))(*values)
            keepalive.append(density_buf)
        ptr = (
            ctypes.cast(density_buf, ctypes.POINTER(ctypes.c_float))
            if density_buf is not None
            else None
        )
        count = len(density) if density is not None else 0
        if density_per_face:
            c_guidance.face_density = ptr
            c_guidance.face_density_count = count
        else:
            c_guidance.vertex_density = ptr
            c_guidance.vertex_density_count = count
        status = lib.cyber_remesh_guided(
            mesh.handle,
            ctypes.byref(c_params),
            ctypes.byref(c_guidance),
            progress_cb,
            cancel_cb,
            warning_cb,
            None,
            ctypes.byref(out_handle),
        )
    _check(status)

    if not out_handle.value:
        raise CyberError(_ffi.STATUS_ERROR, _last_error() or "remesh produced no mesh")

    result = Mesh(handle=out_handle.value)
    result.guidance_warnings = list(guidance_warnings)
    for message in guidance_warnings:
        warnings.warn(f"cyberremesh guidance: {message}", stacklevel=2)
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
    CURVATURE = _ffi.BAKE_CURVATURE
    CAVITY = _ffi.BAKE_CAVITY


@dataclass
class BakeParams:
    """Bake settings (mirror of ``CyberBakeParams``)."""

    width: int = 512
    height: int = 512
    cage_distance: float = 0.1
    ao_samples: int = 16
    ao_radius: float = 1.0
    #: Curvature magnitude (1/length) saturating CURVATURE/CAVITY to full
    #: white/black. 0 = auto (95th percentile of |curvature| on the Target).
    curvature_range: float = 0.0

    def _to_c(self) -> "_ffi.CyberBakeParams":
        return _ffi.CyberBakeParams(
            width=int(self.width),
            height=int(self.height),
            cage_distance=float(self.cage_distance),
            ao_samples=int(self.ao_samples),
            ao_radius=float(self.ao_radius),
            curvature_range=float(self.curvature_range),
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
