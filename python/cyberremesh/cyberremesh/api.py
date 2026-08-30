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
    "Document",
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
    "LoopSubdivideMode",
    "UnsupportedTopologyError",
    "Snapper",
    "SnapReport",
    "SoftTransformReport",
    "SeamCostParams",
    "SeamSet",
    "SeamPath",
    "HandoffInfo",
    "IncompatibleVersionError",
    "FieldEvaluator",
    "bake_field",
    "ConformReport",
    "conform",
    "builtin_presets",
    "ExportPreset",
    "PresetMapEntry",
    "BundleFile",
    "BundleResult",
    "write_bundle",
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
        # ZRemesher-class: the quad-cover path with the explicit topology-layout
        # stage on. See docs/zremesher-plan.md.
        "zremesher": 4,
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
class IsotropicParams:
    """Adaptive isotropic (triangle) remeshing parameters.

    Mirror of the useful subset of ``cyber::remesh::IsotropicOptions``.
    ``target_edge_length`` has no default: it is a WORLD-SPACE length, so no
    value means anything until you have seen the mesh. Size it against the
    model (a bounding-box diagonal over the number of edges you want across it
    is the usual choice) — the engine rejects a non-positive one rather than
    inventing a scale.

    The painted-density field ``IsotropicOptions`` carries is not mirrored
    here; painted density stays reachable through :func:`remesh`'s ``density``
    guidance until the isotropic entry point grows the same array-validation
    surface.
    """

    target_edge_length: float  # world-space, must be > 0
    iterations: int = 3  # split/collapse/flip/smooth rounds
    adaptivity: float = 0.0  # 0 uniform .. 1 fully curvature-adaptive
    # 0: flat closest-point projection. > 0: PN-triangle (curved) projection
    # with this crease threshold, so relaxed vertices follow the smooth surface
    # instead of sinking onto coarse facets.
    smooth_normal_degrees: float = 0.0
    # Dihedral threshold used to tag feature edges BEFORE remeshing, so creases
    # and boundaries survive it. <= 0 skips the tagging pass and honours
    # whatever the mesh already carries.
    sharp_edge_degrees: float = 90.0

    def _to_c(self) -> "_ffi.CyberIsotropicParams":
        return _ffi.CyberIsotropicParams(
            target_edge_length=float(self.target_edge_length),
            iterations=int(self.iterations),
            adaptivity=float(self.adaptivity),
            smooth_normal_degrees=float(self.smooth_normal_degrees),
            sharp_edge_degrees=float(self.sharp_edge_degrees),
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
class UnwrapSeamsParams:
    """Parameters for :meth:`Mesh.unwrap_seams`.

    Only the knobs that still mean something once the caller supplies the
    seams. :class:`AtlasParams`' chart-angle and chart-merge fields are absent
    on purpose: those decide WHERE to cut, and here the seam set already has.
    """

    pack_margin: float = 0.0  # gap around each island, in UV units
    texture_size: int = 1024  # resolution for the texel-density readout
    # Rotate each chart to its minimum-area bounding box before packing.
    reorient_charts: bool = True

    def _to_c(self) -> "_ffi.CyberUnwrapSeamsParams":
        return _ffi.CyberUnwrapSeamsParams(
            pack_margin=float(self.pack_margin),
            texture_size=int(self.texture_size),
            reorient_charts=1 if self.reorient_charts else 0,
        )


@dataclass
class AtlasResult:
    """Aggregate atlas quality/packing report (mirror of ``CyberAtlasResult``)."""

    #: Charts that occupy area in the packed atlas.
    chart_count: int = 0
    seam_edges: int = 0
    max_angle_distortion: float = 0.0
    rms_angle_distortion: float = 0.0
    flipped_charts: int = 0
    fallback_charts: int = 0
    #: Fraction of the unit square the chart geometry covers (texel efficiency).
    packed_area: float = 0.0
    texel_density: float = 0.0
    #: Degenerate islands that cover nothing; ``chart_count + dropped_charts``
    #: is the number of islands the seams cut the mesh into.
    dropped_charts: int = 0
    #: Fraction covered by the charts' bounding boxes (packer tightness).
    packed_box_area: float = 0.0

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
            dropped_charts=int(c.dropped_charts),
            packed_box_area=float(c.packed_box_area),
        )


class Falloff:
    """Soft-selection falloff curves (mirrors ``CyberFalloff``)."""

    LINEAR = _ffi.FALLOFF_LINEAR
    SMOOTH = _ffi.FALLOFF_SMOOTH
    SHARP = _ffi.FALLOFF_SHARP
    ROUND = _ffi.FALLOFF_ROUND


class Subdivision:
    """Subdivision modes (mirrors ``CyberSubdivisionMode``).

    Both modes produce the same topology — every n-gon becomes n quads around
    its centroid — and differ only in where the new vertices land.
    """

    #: Vertices stay put, edge points at the midpoint: resolution, no
    #: curvature. The default, and what :meth:`Mesh.subdivide` has always done.
    LINEAR = _ffi.SUBDIV_LINEAR
    #: Catmull-Clark smooth rules, so the cage converges toward its limit
    #: surface with no Target to reproject onto. Creases — feature-tagged,
    #: boundary and non-manifold edges — use the sharp rule, so open borders
    #: keep their shape and corners stay put.
    CATMULL_CLARK = _ffi.SUBDIV_CATMULL_CLARK


class LoopSubdivideMode:
    """How :meth:`Mesh.loop_subdivide` places vertices (mirrors
    ``CyberLoopSubdivideMode``).

    Both modes produce the same topology — one triangle becomes four. They
    differ only in where the vertices land, and that difference is never
    inferred: a caller who wants resolution without a shape change has to say
    so, and one who wants smoothing has to ask for it.
    """

    #: True Loop weights. The surface converges to the Loop limit surface, so
    #: THE SHAPE CHANGES — a faceted cage visibly rounds off.
    SMOOTH = _ffi.LOOP_SUBDIVIDE_SMOOTH
    #: Pure 1-to-4 split: new vertices are exact edge midpoints and no original
    #: vertex moves. More polygons, same shape.
    LINEAR = _ffi.LOOP_SUBDIVIDE_LINEAR


@dataclass(frozen=True)
class SoftTransformReport:
    """Outcome of a weighted transform/relax.

    Both counts are DISTINCT VERTICES, never per-iteration writes:
    :attr:`moved` never exceeds the mesh's vertex count even for a
    multi-iteration :meth:`Mesh.relax_selection` that revisits each of them
    every sweep, and :attr:`resnapped` never exceeds :attr:`moved`.
    """

    #: Distinct vertices the op wrote (weight > 0 and not pinned).
    moved: int
    #: Distinct moved vertices the Target re-projection pulled back strictly
    #: further than ``resnap_epsilon``. With the default epsilon of 0 a vertex
    #: the projection did not have to correct at all (correction exactly 0 —
    #: it already sat on the Target, which is what happens when the weight is
    #: so small that the blended target is bit-identical to the current
    #: position) is not counted, so ``moved - resnapped`` is the number of
    #: vertices that were already on-surface.
    resnapped: int
    #: Largest counted correction.
    max_snap_distance: float

    @classmethod
    def _from_c(cls, c: "_ffi.CyberSoftTransformReport") -> "SoftTransformReport":
        return cls(
            moved=int(c.moved),
            resnapped=int(c.resnapped),
            max_snap_distance=float(c.max_snap_distance),
        )


@dataclass(frozen=True)
class SnapReport:
    """Outcome of :meth:`Mesh.snap_all` — the whole-mesh Target projection."""

    #: Live, unpinned vertices the projection actually displaced. A vertex
    #: already sitting on the Target is not counted, so this is "how much work
    #: was left", not the vertex count.
    moved: int
    #: Largest single displacement among those, 0.0 when nothing moved.
    max_distance: float


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


def _float_buffer(values) -> "ctypes.Array":
    """A packed ``c_float`` array from an ``(n, 3)`` or already-flat sequence."""
    if HAVE_NUMPY:
        flat = _np.ascontiguousarray(_np.asarray(values, dtype=_np.float32).reshape(-1))
        buf = (ctypes.c_float * int(flat.size))()
        ctypes.memmove(buf, flat.ctypes.data, int(flat.nbytes))
        return buf
    flat_list: List[float] = []
    for item in values:
        if isinstance(item, (int, float)):
            flat_list.append(float(item))
        else:
            flat_list.extend(float(v) for v in item)
    return (ctypes.c_float * len(flat_list))(*flat_list)


def _uint32_buffer(values) -> "ctypes.Array":
    """A packed ``c_uint32`` array from an ``(n, k)`` or already-flat sequence."""
    if HAVE_NUMPY:
        flat = _np.ascontiguousarray(_np.asarray(values, dtype=_np.uint32).reshape(-1))
        buf = (ctypes.c_uint32 * int(flat.size))()
        ctypes.memmove(buf, flat.ctypes.data, int(flat.nbytes))
        return buf
    flat_list: List[int] = []
    for item in values:
        if isinstance(item, int):
            flat_list.append(item)
        else:
            flat_list.extend(int(v) for v in item)
    return (ctypes.c_uint32 * len(flat_list))(*flat_list)


def _ids(values: Optional[Sequence[int]]) -> Tuple[Optional["ctypes.Array"], int]:
    """A ``(buffer, count)`` pair for an optional id list, NULL/0 when empty.

    "No ids" is spelled NULL with count 0 throughout the C ABI (empty pin
    sets, empty face lists), which is what the header documents; handing it a
    zero-length array instead relies on ctypes' pointer for an empty buffer
    being usable, so this returns the documented spelling.
    """
    ids = [int(v) for v in (values or [])]
    if not ids:
        return None, 0
    return (ctypes.c_uint32 * len(ids))(*ids), len(ids)


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
    edge". Defaults bias the route toward feature-tagged and creased edges so a
    routed seam follows the hard edge instead of the flat shortcut. Valleys
    (``concave_weight``) and ridges (``convex_weight``) are tuned separately —
    ``crease_degrees`` is compared against the dihedral's MAGNITUDE. The convex
    default is much closer to neutral because a seam hides better in a valley:
    a ridge is taken over flat ground but never pulls the route off a valley
    that is available. Lower it toward ``concave_weight`` for aggressive
    ridge-following on convex-creased CAD parts.
    """

    flat_weight: float = 1.0
    feature_weight: float = 0.25
    concave_weight: float = 0.35
    convex_weight: float = 0.8
    crease_degrees: float = 20.0
    min_weight: float = 1e-3

    def _to_c(self) -> "_ffi.CyberSeamPathOptions":
        return _ffi.CyberSeamPathOptions(
            flat_weight=float(self.flat_weight),
            feature_weight=float(self.feature_weight),
            concave_weight=float(self.concave_weight),
            convex_weight=float(self.convex_weight),
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
            convex_weight=float(c.convex_weight),
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

    Unlike :class:`Snapper`, which copies what it needs, the engine keeps a
    BORROWED reference to the mesh: the mesh must outlive the path. The wrapper
    holds that reference for you, so a path built from an otherwise unreferenced
    mesh stays valid; closing the mesh by hand still invalidates the path, and
    every path method then raises instead of reading freed memory. The path also
    caches vertex ids, so recreate it if that mesh is edited.
    """

    __slots__ = ("_mesh",)
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
        self._mesh = mesh

    @property
    def handle(self) -> int:
        if self._mesh._handle is None:
            raise ValueError("operation on a SeamPath whose Mesh was closed")
        return super().handle

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

    __slots__ = ("_handle", "_stats", "guidance_warnings", "zremesher_report")

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
        # The ZRemesher run report (see remesh()), or None when the mesh did
        # not come from that path. Always present so callers can test it
        # without hasattr.
        self.zremesher_report: Optional["ZRemesherReport"] = None

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

    def copy(self) -> "Mesh":
        """An independent in-memory duplicate of this mesh.

        Exact and lossless — unlike a :meth:`save_obj` / :meth:`load_obj` round
        trip, which narrows to the OBJ text precision and so reports spurious
        vertex movement in a before/after comparison. The copy carries the
        whole handle: geometry, statistics, the hidden-face and tagged-edge
        overlays, the soft-selection weight field and its saved slots. Element
        ids are preserved, so ids collected against this mesh still address the
        same elements in the copy.
        """
        out = ctypes.c_void_p()
        _check(_ffi.get_lib().cyber_mesh_clone(self.handle, ctypes.byref(out)))
        copy = type(self)(handle=out.value)
        copy._stats = self._stats
        copy.guidance_warnings = list(self.guidance_warnings)
        copy.zremesher_report = self.zremesher_report
        return copy

    def __copy__(self) -> "Mesh":
        return self.copy()

    # -- I/O ----------------------------------------------------------------
    @classmethod
    def load(cls, path: str) -> "Mesh":
        """Load a mesh, dispatching on the file extension.

        Readable formats: ``.obj``, ``.ply``, ``.stl``, ``.gltf``, ``.glb``,
        ``.fbx``. Raises :class:`CyberError` naming the path for an unknown
        extension or an unreadable file.
        """
        out = ctypes.c_void_p()
        status = _ffi.get_lib().cyber_mesh_load(
            str(path).encode("utf-8"), ctypes.byref(out)
        )
        if status != _ffi.STATUS_OK:
            raise CyberError(status, _last_error())
        return cls(handle=out.value)

    def save(self, path: str) -> None:
        """Write this mesh, dispatching on the file extension.

        Writable formats: ``.obj`` (with a sibling ``.mtl``), ``.ply``,
        ``.stl``, ``.gltf``, ``.glb``. FBX is **import-only** — writing it needs
        the proprietary binary container, so a ``.fbx`` path raises
        :class:`CyberError`.
        """
        _check(
            _ffi.get_lib().cyber_mesh_save(self.handle, str(path).encode("utf-8"))
        )

    @classmethod
    def load_obj(cls, path: str) -> "Mesh":
        """Alias of :meth:`load`, kept for callers written before the loader
        grew past OBJ. It has always dispatched on the extension."""
        return cls.load(path)

    def save_obj(self, path: str) -> None:
        """Alias of :meth:`save`, kept for callers written before the writer
        grew past OBJ. It has always dispatched on the extension."""
        self.save(path)

    # -- editing ------------------------------------------------------------
    def subdivide(
        self,
        project_to: Optional["Mesh"] = None,
        mode: int = Subdivision.LINEAR,
    ) -> int:
        """Subdivide this mesh IN PLACE, returning the resulting face count.

        Every n-gon is split into n quads around its centroid, so a quad mesh
        of N faces becomes 4N quads and a triangle becomes 3 quads — that is
        the topology in both modes.

        With the default :attr:`Subdivision.LINEAR` there is no smoothing: the
        new vertices land on the existing facets, which adds resolution but not
        curvature. Pass ``project_to`` to recover it — every vertex of the
        subdivided mesh is then projected onto that mesh's surface, which is
        what "subdivide + reproject" means: subdivide the retopologised cage,
        then pull it back onto the original scan.

        With :attr:`Subdivision.CATMULL_CLARK` the smooth rules run instead, so
        the cage rounds toward its limit surface without needing a Target at
        all. Creases — feature-tagged edges plus boundary and non-manifold
        edges — use the sharp rule, so an open patch keeps its border and
        corners stay put. ``project_to`` still applies afterwards if given.

        The mesh is rebuilt from scratch, so every vertex, edge and face id
        from before the call is invalid afterwards.
        """
        lib = _ffi.get_lib()
        faces = ctypes.c_size_t(0)
        snapper = ctypes.c_void_p()
        if project_to is not None:
            _check(lib.cyber_snapper_create(project_to.handle, ctypes.byref(snapper)))
        try:
            _check(
                lib.cyber_retopo_subdivide_ex(
                    self.handle, snapper, int(mode), ctypes.byref(faces)
                )
            )
        finally:
            if snapper:
                lib.cyber_snapper_free(snapper)
        self._stats = None
        return faces.value

    # -- retopology mesh operations -----------------------------------------
    #
    # The whole-mesh commands and the gesture ops that need no stroke geometry
    # (the stroke/drawing family still lives behind the C ABI only). Element-id
    # stability is the contract callers have to read, so each docstring says
    # what it does to ids; :attr:`stats` is dropped by the ops that change
    # topology, because it describes the run that produced the old mesh.



    def loop_subdivide(
        self,
        mode: int = LoopSubdivideMode.SMOOTH,
        project_to: Optional["Mesh"] = None,
    ) -> int:
        """Loop-subdivide this TRIANGLE mesh IN PLACE, returning the face count.

        Triangles in, triangles out: every triangle becomes four, so a mesh of
        N triangles comes back with 4N. This is the triangle counterpart of
        :meth:`subdivide`, which applies Catmull-Clark topology and would turn
        each triangle into three *quads* instead.

        ``mode`` decides whether vertices move, and nothing infers it:

        * :attr:`LoopSubdivideMode.SMOOTH` (the default) uses the true Loop
          weights, so the surface converges to the Loop limit surface — **the
          shape changes**, a faceted cage visibly rounds off.
        * :attr:`LoopSubdivideMode.LINEAR` is a pure 1-to-4 split: new vertices
          are exact edge midpoints and no original vertex moves. Ask for this
          when you want more polygons and the same shape.

        Open meshes keep their border either way: boundary edge points are
        plain midpoints and boundary vertices follow the 1/8, 3/4, 1/8 curve
        rule, never the interior mask that would pull the border inward.

        A face that is not a triangle raises :class:`UnsupportedTopologyError`
        naming the face and its side count. It is deliberately not
        fan-triangulated for you — that is a topology decision only you can
        make; call :meth:`triangulate` first if that is what you want.

        Pass ``project_to`` to reproject every vertex onto that mesh's surface
        afterwards, exactly as :meth:`subdivide` does.

        The mesh is rebuilt from scratch, so every vertex, edge and face id
        from before the call is invalid afterwards.
        """
        lib = _ffi.get_lib()
        faces = ctypes.c_size_t(0)
        snapper = ctypes.c_void_p()
        if project_to is not None:
            _check(lib.cyber_snapper_create(project_to.handle, ctypes.byref(snapper)))
        try:
            status = lib.cyber_retopo_loop_subdivide(
                self.handle, int(mode), snapper, ctypes.byref(faces)
            )
        finally:
            if snapper:
                lib.cyber_snapper_free(snapper)
        if status != _ffi.STATUS_OK:
            _raise_status(status)
        self._stats = None
        return faces.value

    def isotropic_remesh(self, params: "IsotropicParams | float") -> int:
        """Adaptive isotropic (triangle) remesh IN PLACE, returning the face count.

        This is the "give me more polygons to work with" operation, and the
        only way to reach the engine's isotropic stage without running the
        whole quad pipeline. Per iteration it splits long edges, collapses
        short ones, flips toward valence 6, tangentially smooths and projects
        back onto the input surface, so edge lengths converge to
        ``[4/5, 4/3] * target_edge_length``. A target BELOW the mesh's current
        edge length densifies it; one above decimates it.

        ``params`` is an :class:`IsotropicParams`, or a bare float read as the
        target edge length when the defaults will do::

            mesh.isotropic_remesh(0.05)
            mesh.isotropic_remesh(IsotropicParams(0.05, adaptivity=1.0))

        THE RESULT IS A TRIANGLE MESH. A non-triangulated input is
        triangulated first rather than refused — the obvious call is to
        densify quads that just came out of :func:`remesh`, and the isotropic
        passes cannot preserve quads in any case.

        The projection reference is built from the input surface before any
        remeshing, so the result stays on the shape you passed in. Feature
        edges are tagged from ``sharp_edge_degrees`` first and are never
        collapsed, flipped, smoothed off or projected.

        The mesh is rewritten in place, so every vertex, edge and face id from
        before the call is meaningless afterwards. The call cannot be
        interrupted; size the target before making it (halving the target
        roughly quadruples the face count).
        """
        if not isinstance(params, IsotropicParams):
            params = IsotropicParams(float(params))
        c_params = params._to_c()
        faces = ctypes.c_size_t(0)
        _check(
            _ffi.get_lib().cyber_mesh_isotropic_remesh(
                self.handle, ctypes.byref(c_params), ctypes.byref(faces)
            )
        )
        self._stats = None
        return faces.value

    def triangulate(self) -> int:
        """Fan-triangulate every face with more than three sides, IN PLACE.

        Returns the resulting face count. Vertex and edge ids survive, and so
        do the ids of the faces that get split, but each n-gon gains NEW face
        ids for its extra triangles: vertex- and edge-keyed annotations stay
        valid while face-keyed ones become partial. A mesh with no faces
        raises :class:`CyberError`.
        """
        faces = ctypes.c_size_t(0)
        _check(
            _ffi.get_lib().cyber_retopo_triangulate(self.handle, ctypes.byref(faces))
        )
        self._stats = None
        return faces.value

    def relax(
        self,
        center: Optional[Sequence[float]] = None,
        radius: float = 0.0,
        strength: float = 0.5,
        iterations: int = 1,
        auto_pin_corners: bool = False,
        pinned: Optional[Sequence[int]] = None,
        snapper: Optional["Snapper"] = None,
    ) -> None:
        """Tangential Laplacian smoothing, brushed or over the whole mesh.

        Give ``center`` to confine the smoothing to a brush of ``radius``
        around it; leave it ``None`` to relax EVERY vertex — the ABI has no
        separate relax-all call, a non-positive radius is how you ask for one,
        so ``radius`` is ignored in that case rather than quietly turning the
        call back into a brush at the origin.

        ``strength`` is the 0..1 per-iteration blend and ``iterations`` must be
        at least 1; anything else raises :class:`CyberError`. Vertices in
        ``pinned`` are held, and ``auto_pin_corners`` additionally holds
        low-valence grid corners so regular patch shapes survive. A ``snapper``
        reprojects the vertices it moves onto that Target inside the same pass.

        Positions only: every vertex, edge and face id survives.
        """
        whole_mesh = center is None
        brush = _vec3((0.0, 0.0, 0.0) if whole_mesh else center)
        pin_buf, pin_count = _ids(pinned)
        _check(
            _ffi.get_lib().cyber_retopo_relax(
                self.handle, brush, 0.0 if whole_mesh else float(radius),
                float(strength), int(iterations), 1 if auto_pin_corners else 0,
                pin_buf, pin_count, snapper.handle if snapper else None,
            )
        )

    def snap_all(
        self, snapper: Optional["Snapper"], pinned: Optional[Sequence[int]] = None
    ) -> SnapReport:
        """Project every live vertex onto a Target surface.

        ``snapper`` has no default and a missing or empty one raises
        :class:`CyberError` — there is nothing to snap to otherwise, and the
        ABI refuses rather than quietly doing nothing. ``None`` is passed
        through so that refusal comes from the engine, with its message.
        Vertices listed in ``pinned`` are left exactly where they are.

        Positions only: every vertex, edge and face id survives, so
        annotations keyed on ids (pins, loop tags, hidden faces) stay valid.
        """
        pin_buf, pin_count = _ids(pinned)
        moved = ctypes.c_size_t(0)
        max_distance = ctypes.c_float(0.0)
        _check(
            _ffi.get_lib().cyber_retopo_snap_all(
                self.handle, snapper.handle if snapper else None, pin_buf,
                pin_count, ctypes.byref(moved), ctypes.byref(max_distance),
            )
        )
        return SnapReport(moved=moved.value, max_distance=max_distance.value)

    def delete_faces(self, faces: Sequence[int]) -> int:
        """Delete the listed faces, then any vertices left isolated.

        Dead and out-of-range face ids are SKIPPED rather than failing, so the
        return value is how many were actually removed, not ``len(faces)``.
        The removed faces, and the edges and vertices that went with them, are
        dead ids afterwards.
        """
        buf, count = _ids(faces)
        removed = ctypes.c_size_t(0)
        _check(
            _ffi.get_lib().cyber_retopo_delete_faces(
                self.handle, buf, count, ctypes.byref(removed)
            )
        )
        self._stats = None
        return removed.value

    def dissolve_edges(self, edges: Sequence[int]) -> int:
        """Dissolve interior edges, returning how many actually dissolved.

        Each listed edge with exactly two live faces is removed and its faces
        merged into one, so a triangle pair becomes a quad. Dead, boundary and
        would-be-degenerate edges are SKIPPED, as are edges invalidated by an
        earlier dissolve in the same call — dissolving none is a success
        returning 0, not an error.

        Each merge retires the dissolved edge and BOTH its faces and creates a
        new face, so face ids gathered before the call may be dead afterwards.
        """
        buf, count = _ids(edges)
        dissolved = ctypes.c_size_t(0)
        _check(
            _ffi.get_lib().cyber_retopo_dissolve_edges(
                self.handle, buf, count, ctypes.byref(dissolved)
            )
        )
        self._stats = None
        return dissolved.value

    def insert_loop(self, edge: int, t: float = 0.5) -> int:
        """Insert a COMPLETE edge loop around the quad ring through ``edge``.

        Every edge of the ring is split at ``t`` (strictly inside ``(0, 1)``;
        0.5 = midpoints) and every ring quad is split between consecutive
        midpoints; a lone quad degenerates to the one-quad insert. Returns the
        number of NEW faces. An edge that is dead or borders no quad, or a
        ``t`` outside ``(0, 1)``, raises :class:`CyberError` with the mesh
        unchanged.

        The split faces and edges are replaced, so face and edge ids gathered
        before the call may be dead; existing vertex ids survive and the
        midpoints are added as new ones.
        """
        new_faces = ctypes.c_size_t(0)
        _check(
            _ffi.get_lib().cyber_retopo_insert_loop(
                self.handle, int(edge), float(t), ctypes.byref(new_faces)
            )
        )
        self._stats = None
        return new_faces.value

    def merge_vertices(self, keep: int, remove: int, at_midpoint: bool = False) -> None:
        """Merge vertex ``remove`` into vertex ``keep``.

        Faces degenerated by the merge are deleted. With ``at_midpoint`` the
        survivor moves to the pair's midpoint, otherwise it stays at ``keep``'s
        position. A dead vertex on either side, or ``keep == remove``, raises
        :class:`CyberError` with the mesh unchanged.

        ``keep`` survives; ``remove`` and every face and edge the merge
        collapsed are dead ids afterwards.
        """
        _check(
            _ffi.get_lib().cyber_retopo_merge_vertices(
                self.handle, int(keep), int(remove), 1 if at_midpoint else 0
            )
        )
        self._stats = None

    def rotate_edge(self, edge: int) -> None:
        """Rotate an interior edge.

        A triangle pair flips its shared diagonal; a quad pair is re-split one
        ring corner over, turning the pair's loop-flow direction. An edge that
        is dead, on the boundary, part of a pair that is neither two triangles
        nor two quads, or whose rotation would fold the mesh, raises
        :class:`CyberError` with the mesh unchanged.

        The quad case rebuilds the pair, so both face ids and the rotated
        edge's id may be dead afterwards; vertex ids always survive.
        """
        _check(_ffi.get_lib().cyber_retopo_rotate_edge(self.handle, int(edge)))
        self._stats = None

    @classmethod
    def load_handoff(cls, path: str) -> Tuple["Mesh", "HandoffInfo"]:
        """Open a versioned sculpt handoff as a Target.

        Reads the PLY (or glTF) profile documented in
        ``docs/sculpt-handoff-format.md``. Returns the mesh and what the
        handoff declared. A version this engine does not support raises
        :class:`IncompatibleVersionError` naming both versions; no partial mesh
        is ever produced.
        """
        out = ctypes.c_void_p()
        info = _ffi.CyberHandoffInfo()
        status = _ffi.get_lib().cyber_handoff_open(
            str(path).encode("utf-8"), ctypes.byref(out), ctypes.byref(info)
        )
        if status != _ffi.STATUS_OK:
            _raise_status(status)
        return cls(handle=out.value), HandoffInfo._from_c(info)

    @classmethod
    def load_handoff_buffers(
        cls,
        positions,
        indices,
        normals=None,
        colors=None,
        material_mix=None,
        version: Optional[Tuple[int, int]] = None,
        producer: str = "",
    ) -> Tuple["Mesh", "HandoffInfo"]:
        """Open an IN-MEMORY sculpt handoff as a Target — no intermediate file.

        The second profile documented in ``docs/sculpt-handoff-format.md``:
        plain arrays instead of a PLY. ``positions`` is ``(n, 3)`` (or flat)
        and ``indices`` is ``(m, 3)`` (or flat) — triangles only. The optional
        ``normals`` / ``colors`` (both ``(n, 3)``, colours in [0, 1]) and
        ``material_mix`` (``(n,)``) carry the same payloads the file profile
        does. ``version`` defaults to :data:`HANDOFF_VERSION`.

        The version gate is the file profile's, so an in-process producer
        cannot bypass it: an unsupported version raises
        :class:`IncompatibleVersionError` naming both versions.
        """
        pos = _float_buffer(positions)
        if len(pos) % 3 != 0:
            raise ValueError("load_handoff_buffers: positions must be a multiple of 3 floats")
        vertex_count = len(pos) // 3
        idx = _uint32_buffer(indices)
        if len(idx) % 3 != 0:
            raise ValueError("load_handoff_buffers: indices must be a multiple of 3 (triangles)")

        # The C struct carries pointers with an IMPLIED length, so a short
        # optional array would be read past its end. Checked here, where the
        # length is still known, rather than trusting the caller.
        def optional(values, per_vertex: int, label: str):
            if values is None:
                return None
            buf = _float_buffer(values)
            if len(buf) != vertex_count * per_vertex:
                raise ValueError(
                    "load_handoff_buffers: {0} needs {1} floats for {2} vertices, got {3}".format(
                        label, vertex_count * per_vertex, vertex_count, len(buf)
                    )
                )
            return buf

        nrm = optional(normals, 3, "normals")
        col = optional(colors, 3, "colors")
        mix = optional(material_mix, 1, "material_mix")

        def as_float_ptr(buf):
            return ctypes.cast(buf, ctypes.POINTER(ctypes.c_float)) if buf is not None else None

        major, minor = HANDOFF_VERSION if version is None else version
        buffers = _ffi.CyberHandoffBuffers()
        buffers.positions = as_float_ptr(pos)
        buffers.normals = as_float_ptr(nrm)
        buffers.colors = as_float_ptr(col)
        buffers.material_mix = as_float_ptr(mix)
        buffers.vertex_count = vertex_count
        buffers.indices = ctypes.cast(idx, ctypes.POINTER(ctypes.c_uint32))
        buffers.index_count = len(idx)
        buffers.version_major = int(major)
        buffers.version_minor = int(minor)
        buffers.producer = producer.encode("utf-8") if producer else None

        out = ctypes.c_void_p()
        info = _ffi.CyberHandoffInfo()
        status = _ffi.get_lib().cyber_handoff_open_buffers(
            ctypes.byref(buffers), ctypes.byref(out), ctypes.byref(info)
        )
        if status != _ffi.STATUS_OK:
            _raise_status(status)
        return cls(handle=out.value), HandoffInfo._from_c(info)

    def unwrap_atlas(self, params: Optional["AtlasParams"] = None) -> "AtlasResult":
        """Generate an automatic UV atlas for this mesh, IN PLACE.

        Seams the mesh into normal-coherent charts, LSCM-unwraps each, packs
        them into the unit square and writes the per-corner ``uv`` attribute, so
        a subsequent :meth:`save_obj` emits ``vt`` / ``f v/vt``. Returns an
        :class:`AtlasResult` with distortion and packing statistics.

        COST: with the default ``max_chart_distortion`` the atlas runs a chart
        merge that trial-unwraps the union of candidate chart pairs. That pass
        dominates the call and takes minutes on a mesh of tens of thousands of
        faces; this binding cannot be interrupted while it runs. Set
        ``AtlasParams(max_chart_distortion=0.0)`` to bound the unwrap to
        milliseconds, at the cost of more charts.
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

    def unwrap_seams(
        self, seams: "SeamSet", params: Optional["UnwrapSeamsParams"] = None
    ) -> "AtlasResult":
        """Unwrap this mesh along `seams`, IN PLACE — the counterpart to
        :meth:`unwrap_atlas`.

        :meth:`unwrap_atlas` decides its own cuts and ignores whatever you
        marked. This one takes the seam set YOU built — by
        :meth:`SeamSet.mark`, or by committing a routed :class:`SeamPath` into
        it — cuts the mesh into islands at exactly those edges, LSCM-unwraps
        each (planar projection as fallback), optionally re-orients them and
        packs them into the unit square. Everything downstream of the cut is
        the same code the automatic atlas runs, so the :class:`AtlasResult`
        means the same thing: ``chart_count`` is the number of islands the
        seams cut the mesh into, ``seam_edges`` the size of the set you passed.

        An EMPTY seam set means "do not cut" — a closed mesh comes back as one
        chart — never "seam it automatically".

        >>> seams, path = SeamSet(), SeamPath(mesh)
        >>> path.add_waypoint(a); path.add_waypoint(b)
        >>> path.commit(seams)
        >>> mesh.unwrap_seams(seams).chart_count
        2
        """
        if params is None:
            params = UnwrapSeamsParams()
        c_params = params._to_c()
        c_result = _ffi.CyberAtlasResult()
        _check(
            _ffi.get_lib().cyber_uv_unwrap_seams(
                self.handle, seams.handle, ctypes.byref(c_params), ctypes.byref(c_result)
            )
        )
        return AtlasResult._from_c(c_result)

    def stitch_seams(self, seams: "SeamSet", edges: "Sequence[int]") -> None:
        """Sew `edges` shut: the inverse of the cut :meth:`unwrap_seams` applies.

        Each edge is removed from `seams` (so it stops cutting islands) and,
        across it, the two corners at each shared endpoint are welded to the
        average of their UVs so the boundary fits together. Both this mesh and
        `seams` are modified. An edge that is not currently a seam is ignored.
        """
        ids = list(edges)
        buf = (ctypes.c_uint32 * len(ids))(*[int(e) for e in ids]) if ids else None
        _check(
            _ffi.get_lib().cyber_uv_stitch_seams(
                self.handle, seams.handle, buf, len(ids)
            )
        )

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
        # The C side reads exactly 4 floats per sample from a pointer whose
        # length is implied, so a narrower sample would be read past the end of
        # this buffer. Checked here, where the real length is still known.
        if len(flat) % 4 != 0:
            raise ValueError(
                "paint_selection_stroke: samples must be (x, y, z, pressure), "
                "got {0} floats".format(len(flat))
            )
        buf = (ctypes.c_float * len(flat))(*flat)
        _check(
            _ffi.get_lib().cyber_retopo_selection_paint_stroke(
                self.handle, buf, len(flat) // 4, float(radius),
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
        pinned: Optional[Sequence[int]] = None,
    ) -> SoftTransformReport:
        """Apply a 12-float affine per vertex scaled by its selection weight.

        ``transform`` is the column-major 3x3 linear part followed by the
        translation, matching ``cyber_retopo_transform_vertices``.

        ``pinned`` holds vertex ids that are skipped whatever their weight, so
        a pinned vertex is neither moved nor re-snapped and its position is
        bit-identical after the call.
        """
        buf = (ctypes.c_float * 12)(*[float(v) for v in transform])
        pins = pinned or []
        pin_buf = (ctypes.c_uint32 * len(pins))(*[int(p) for p in pins]) if pins else None
        report = _ffi.CyberSoftTransformReport()
        _check(
            _ffi.get_lib().cyber_retopo_selection_transform_pinned(
                self.handle, buf, pin_buf, len(pins), snapper.handle if snapper else None,
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

    def edge_count(self) -> int:
        """Number of edges. Edge ids are dense in ``range(edge_count())``.

        Seam work needs this: :meth:`SeamSet.mark` takes an edge id, and
        without a count there is no way to enumerate the edges to choose from.
        """
        return int(_ffi.get_lib().cyber_mesh_edge_count(self.handle))

    def edge_between(self, a: int, b: int) -> Optional[int]:
        """The edge joining vertices `a` and `b`, or None if they share none."""
        for e in range(self.edge_count()):
            ends = self.edge_endpoints(e)
            if ends is not None and set(ends) == {int(a), int(b)}:
                return e
        return None

    def edge_endpoints(self, edge: int) -> Optional[Tuple[int, int]]:
        """The two vertex ids of a live edge, or ``None`` when it is not alive.

        This is how an edge id becomes drawable geometry: the seam APIs
        (:meth:`SeamSet.edges`, :meth:`SeamPath.edges`) hand back edge ids, and
        pairing this with :meth:`vertex_position` resolves each one to a
        segment — which is the only way to render a COMMITTED seam, since a
        committed seam is a set of edge ids and nothing else.
        """
        buf = (ctypes.c_uint32 * 2)()
        if _ffi.get_lib().cyber_mesh_edge_endpoints(self.handle, int(edge), buf) != 1:
            return None
        return int(buf[0]), int(buf[1])

    def vertex_position(self, vertex: int) -> Optional[Tuple[float, float, float]]:
        """Position of a live vertex BY ID, or ``None`` when it is not alive.

        Prefer this over indexing :attr:`positions` with a vertex id: that
        array is in compacted order, which only coincides with the stable ids
        while no vertex has been removed.
        """
        buf = (ctypes.c_float * 3)()
        if _ffi.get_lib().cyber_mesh_vertex_position(self.handle, int(vertex), buf) != 1:
            return None
        return float(buf[0]), float(buf[1]), float(buf[2])

    def _copy_positions(self) -> "ctypes.Array":
        lib = _ffi.get_lib()
        needed = int(lib.cyber_mesh_copy_positions(self.handle, None, 0))
        buf = (ctypes.c_float * needed)()
        if needed:
            lib.cyber_mesh_copy_positions(self.handle, buf, needed)
        return buf

    def set_positions(self, values) -> None:
        """Write vertex positions back — the exact inverse of :attr:`positions`.

        ``values`` is an ``(n, 3)`` array/sequence (or an already-flat xyz
        sequence) in the same compacted order :attr:`positions` returns, and
        must hold exactly ``vertex_count`` vertices; a mismatch raises rather
        than silently pairing positions with the wrong vertices. Topology, ids
        and every overlay are untouched, so this restores a snapshot taken from
        :attr:`positions` as long as no vertex was added or removed in between.
        """
        buf = _float_buffer(values)
        expected = self.vertex_count * 3
        if len(buf) != expected:
            raise ValueError(
                "set_positions: expected {0} floats ({1} vertices), got {2}".format(
                    expected, self.vertex_count, len(buf)
                )
            )
        _check(_ffi.get_lib().cyber_mesh_set_positions(self.handle, buf, len(buf)))

    if HAVE_NUMPY:

        @property
        def positions(self):
            """Vertex positions as an ``(n, 3)`` float32 ndarray (a snapshot).

            The engine stores vertices in an index-addressed pool, so this is a
            packed copy rather than a live view into engine memory. Assigning
            to it calls :meth:`set_positions`.
            """
            buf = self._copy_positions()
            arr = _np.frombuffer(buf, dtype=_np.float32).copy()
            return arr.reshape((-1, 3))

        @positions.setter
        def positions(self, values) -> None:
            self.set_positions(values)


class Document:
    """The editing unit that outlives the session.

    Holds the high-poly Target, the low-poly EditMesh and the named
    soft-selection slots, and serializes all three into one versioned byte
    container. This is what makes a saved selection survive a restart: the
    slots on a :class:`Mesh` handle are session state and vanish when the
    handle is closed.

    The seam is explicit and by value. :meth:`set_edit_mesh` copies the mesh's
    geometry *and* its named slots in; :meth:`edit_mesh` hands back a fresh
    :class:`Mesh` carrying those slots. Nothing stays in sync afterwards — edit
    the mesh and set it again. The live (unsaved) weight field is not a slot
    and is not persisted, so call :meth:`Mesh.save_selection` first and
    :meth:`Mesh.load_selection` after the round trip.

        doc = Document()
        mesh.save_selection("north-taper")
        doc.set_edit_mesh(mesh)
        doc.save_file(path)
        ...
        restored = Document.load_file(path).edit_mesh()
        restored.load_selection("north-taper")

    Usable as a context manager.
    """

    __slots__ = ("_handle",)

    def __init__(self, handle: Optional[int] = None):
        if handle is None:
            handle = _ffi.get_lib().cyber_document_create()
            if not handle:
                raise CyberError(_ffi.STATUS_ERROR, _last_error())
        self._handle = handle

    # -- lifetime -----------------------------------------------------------
    @property
    def handle(self) -> int:
        if self._handle is None:
            raise ValueError("operation on a closed Document")
        return self._handle

    def close(self) -> None:
        """Release the underlying engine handle (idempotent)."""
        if self._handle is not None:
            _ffi.get_lib().cyber_document_free(self._handle)
            self._handle = None

    def __del__(self):  # pragma: no cover - GC timing dependent
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Document":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # -- meshes -------------------------------------------------------------
    def set_target(self, mesh: Optional["Mesh"]) -> None:
        """Copy ``mesh`` in as the high-poly Target (``None`` clears it)."""
        _check(
            _ffi.get_lib().cyber_document_set_target(
                self.handle, mesh.handle if mesh else None
            )
        )

    def set_edit_mesh(self, mesh: Optional["Mesh"]) -> None:
        """Copy ``mesh`` in as the EditMesh, named selection slots included."""
        _check(
            _ffi.get_lib().cyber_document_set_edit_mesh(
                self.handle, mesh.handle if mesh else None
            )
        )

    def target(self) -> "Mesh":
        """A fresh :class:`Mesh` owning a copy of the stored Target."""
        return self._mesh(_ffi.get_lib().cyber_document_target(self.handle))

    def edit_mesh(self) -> "Mesh":
        """A fresh :class:`Mesh` with the stored EditMesh and its slots."""
        return self._mesh(_ffi.get_lib().cyber_document_edit_mesh(self.handle))

    def _mesh(self, handle: Optional[int]) -> "Mesh":
        if not handle:
            raise CyberError(_ffi.STATUS_ERROR, _last_error())
        return Mesh(handle=handle)

    # -- slots --------------------------------------------------------------
    def selection_slots(self) -> List[str]:
        """Names of the persisted selection slots, in name order."""
        lib = _ffi.get_lib()
        count = int(lib.cyber_document_slot_count(self.handle))
        names = []
        for i in range(count):
            raw = lib.cyber_document_slot_name(self.handle, i)
            names.append(raw.decode("utf-8") if raw else "")
        return names

    def selection_weights(self, name: str) -> List[float]:
        """The weights of one slot, indexed by EditMesh vertex id."""
        lib = _ffi.get_lib()
        key = name.encode("utf-8")
        needed = int(lib.cyber_document_slot_weights(self.handle, key, None, 0))
        if needed == 0:
            return []
        buf = (ctypes.c_float * needed)()
        lib.cyber_document_slot_weights(self.handle, key, buf, needed)
        return list(buf)

    # -- serialization ------------------------------------------------------
    def save(self) -> bytes:
        """The document as one versioned byte container."""
        lib = _ffi.get_lib()
        needed = int(lib.cyber_document_save(self.handle, None, 0))
        buf = (ctypes.c_uint8 * needed)()
        lib.cyber_document_save(self.handle, buf, needed)
        return bytes(buf)

    @staticmethod
    def load(data: bytes) -> "Document":
        """Parse a byte container produced by :meth:`save`."""
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        handle = _ffi.get_lib().cyber_document_load(buf, len(data))
        if not handle:
            raise CyberError(_ffi.STATUS_ERROR, _last_error())
        return Document(handle=handle)

    def save_file(self, path: str) -> None:
        """Write the byte container to ``path``."""
        _check(_ffi.get_lib().cyber_document_save_file(self.handle, str(path).encode("utf-8")))

    @staticmethod
    def load_file(path: str) -> "Document":
        """Read a document written by :meth:`save_file`."""
        handle = _ffi.get_lib().cyber_document_load_file(str(path).encode("utf-8"))
        if not handle:
            raise CyberError(_ffi.STATUS_ERROR, _last_error())
        return Document(handle=handle)


@dataclass
class FlowGuide:
    """A user-drawn flow guide: an ordered polyline on or near the surface.

    The cross field is softly biased toward the guide's tangent within
    ``radius`` of the stroke, scaled by ``strength`` (clamped to ``[0, 1]``).
    ``radius`` is a world-space distance and must be greater than 0 — a
    zero-radius guide could never be honored, so it is rejected rather than
    silently ignored.

    ``mode`` says what the stroke is ASKING for, and the two requests are
    genuinely different. ``"orientation"`` (the default, and exactly the
    historical behavior) says "line the field up this way around here" — a soft
    bias competing with the smoothness term, so nearby loops lean toward the
    stroke without any one of them being pinned to it. ``"topology"`` says "put
    an actual edge loop HERE": the stroke is projected to an edge path and
    pinned, so it becomes a curve in the layout. An unrecognised mode is an
    error, never a fallback — a typo'd ``"topolgy"`` silently biasing the field
    instead of cutting a loop is the failure this exists to remove.

    ``closed`` marks a polyline that closes back on its first point: what an
    eye, mouth, shoulder, knee or wrist loop is.
    """

    points: Sequence[Sequence[float]]
    strength: float = 1.0
    radius: float = 0.0
    mode: str = "orientation"
    closed: bool = False

    _MODES = {"orientation": 0, "topology": 1}

    def _mode_code(self) -> int:
        try:
            return self._MODES[self.mode]
        except (KeyError, TypeError):
            raise ValueError(
                "FlowGuide.mode must be one of {0}, got {1!r}".format(
                    sorted(self._MODES), self.mode
                )
            ) from None


@dataclass
class ZRemesherParams:
    """The ZRemesher-only controls, mirroring ``CyberZRemesherParams``.

    Separate from :class:`RemeshParams` for the same reason the C structs are
    separate: these apply only to the ZRemesher path, and folding them into the
    canonical parameter set would break every compiled caller of the C ABI.

    Pass one to :func:`remesh` together with ``quad_method="zremesher"``.
    """

    #: ``"fast"`` solves one predicted path. ``"best"`` solves BOTH
    #: cross-field candidates and keeps the one that scores better — no static
    #: "organic vs CAD" threshold picks the right field for every model, so the
    #: answer is to measure both. It costs a second full solve.
    quality: str = "fast"
    #: ``"none"``, ``"x"``, ``"y"`` or ``"z"``. Solves one half and mirrors its
    #: CONNECTIVITY, so the halves are exact reflections rather than merely
    #: similar shapes. ``target_quad_count`` names the WHOLE model.
    symmetry: str = "none"
    #: Size the solve substrate from the unified sizing field. Off by default,
    #: and deliberately: it was MEASURED to make thin-feature survival worse,
    #: which is the one thing it exists to improve. Exposed so the measurement
    #: can be repeated, not because it is recommended.
    unified_sizing: bool = False
    #: Terminate separatrices reaching an open boundary there instead of
    #: abandoning them.
    boundary_chains: bool = True
    #: Recover fold-damaged node rotations by feasible-range projection instead
    #: of containing the node.
    fold_repair: bool = True

    _QUALITY = {"fast": 0, "best": 1}
    _SYMMETRY = {"none": 0, "x": 1, "y": 2, "z": 3}

    def _to_c(self) -> "_ffi.CyberZRemesherParams":
        try:
            quality = self._QUALITY[self.quality]
        except (KeyError, TypeError):
            raise ValueError(
                "quality must be one of {0}, got {1!r}".format(sorted(self._QUALITY), self.quality)
            ) from None
        try:
            symmetry = self._SYMMETRY[self.symmetry]
        except (KeyError, TypeError):
            raise ValueError(
                "symmetry must be one of {0}, got {1!r}".format(
                    sorted(self._SYMMETRY), self.symmetry
                )
            ) from None
        return _ffi.CyberZRemesherParams(
            quality=quality,
            symmetry=symmetry,
            unified_sizing=1 if self.unified_sizing else 0,
            boundary_chains=1 if self.boundary_chains else 0,
            fold_repair=1 if self.fold_repair else 0,
        )


@dataclass
class ZRemesherReport:
    """What a ZRemesher run produced — the layout, the candidate, the symmetry.

    Reachable as :attr:`Mesh.zremesher_report` on the result of a ``remesh``
    call that used ``quad_method="zremesher"``. Every number here was
    previously visible only on the engine's stderr.

    Layout counts are SUMMED over the islands the run solved: a multi-island
    mesh has no single layout, and "how many singularities did this produce"
    means all of them.
    """

    layouts: int = 0
    layouts_valid: int = 0
    nodes: int = 0
    arcs: int = 0
    patches: int = 0
    singularities: int = 0
    t_junctions: int = 0
    feature_arcs: int = 0
    boundary_arcs: int = 0
    excluded_arcs: int = 0
    non_closing_patches: int = 0
    #: Sum of singularity indices; ``4 * Euler characteristic`` for a valid field.
    total_index: int = 0
    #: The cross field ``quality="best"`` kept (``"multires"`` or
    #: ``"single-level"``). Empty under ``"fast"``, which selects nothing — an
    #: empty name means "no selection ran", never "selection failed".
    selected_candidate: str = ""
    quality_score: float = 0.0
    symmetry_applied: bool = False
    #: Checked on the RESULT, not assumed from the construction.
    topologically_symmetric: bool = False
    mirrored_vertices: int = 0
    mirrored_faces: int = 0
    border_snapped: int = 0
    membranes_removed: int = 0
    max_border_drift: float = 0.0

    @classmethod
    def _from_c(cls, c: "_ffi.CyberZRemesherReport") -> "ZRemesherReport":
        return cls(
            layouts=int(c.layouts),
            layouts_valid=int(c.layouts_valid),
            nodes=int(c.layout_nodes),
            arcs=int(c.layout_arcs),
            patches=int(c.layout_patches),
            singularities=int(c.singularities),
            t_junctions=int(c.t_junctions),
            feature_arcs=int(c.feature_arcs),
            boundary_arcs=int(c.boundary_arcs),
            excluded_arcs=int(c.excluded_arcs),
            non_closing_patches=int(c.non_closing_patches),
            total_index=int(c.total_index),
            selected_candidate=c.selected_candidate.decode("utf-8", "replace"),
            quality_score=float(c.quality_score),
            symmetry_applied=bool(c.symmetry_applied),
            topologically_symmetric=bool(c.topologically_symmetric),
            mirrored_vertices=int(c.mirrored_vertices),
            mirrored_faces=int(c.mirrored_faces),
            border_snapped=int(c.border_snapped),
            membranes_removed=int(c.membranes_removed),
            max_border_drift=float(c.max_border_drift),
        )


def remesh(
    mesh: Mesh,
    params: Optional[RemeshParams] = None,
    progress: Optional[Callable[[float, str], None]] = None,
    cancel: Optional[Callable[[], bool]] = None,
    guides: Optional[Sequence[FlowGuide]] = None,
    density: Optional[Sequence[float]] = None,
    density_per_face: bool = False,
    zremesher: Optional[ZRemesherParams] = None,
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

    ``zremesher`` — optional :class:`ZRemesherParams` (quality mode, symmetry
    axis, sizing levers). Requires ``params.quad_method == "zremesher"``; the
    combination is rejected rather than reinterpreted, because silently
    ignoring a symmetry request would hand back an asymmetric mesh. Any run on
    that method attaches a :class:`ZRemesherReport` to the result as
    ``Mesh.zremesher_report``, whether or not ``zremesher`` was supplied.
    """
    if params is None:
        params = RemeshParams()
    is_zremesher = params.quad_method == "zremesher"
    if zremesher is not None and not is_zremesher:
        raise ValueError(
            'zremesher=... requires quad_method="zremesher", got {0!r}'.format(params.quad_method)
        )

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

    # Guidance always travels as CyberGuidanceEx, whose guides carry their mode.
    # The plain CyberFlowGuide has no mode field and cannot grow one without
    # misreading every compiled caller's array, so the "ex" form is what the
    # bindings use — orientation-mode guides through it are byte-identical to
    # the older path.
    #
    # Every buffer below must stay alive across the call, so the point arrays
    # are held in `keepalive` rather than built inline.
    keepalive: List[object] = []
    c_guidance = None
    if guides is not None or density is not None:
        c_guides = (_ffi.CyberFlowGuideEx * len(guides or []))()
        for i, guide in enumerate(guides or []):
            flat = [float(c) for point in guide.points for c in point]
            point_count = len(guide.points)
            # The C ABI reads 3 * point_count floats through a pointer with an
            # implied length, so a point that is not (x, y, z) would be read
            # past the end of this buffer. Rejected here, not in C.
            if len(flat) != 3 * point_count:
                raise ValueError(
                    "remesh: guide {0} needs 3 components per point (x, y, z), "
                    "got {1} floats for {2} points".format(i, len(flat), point_count)
                )
            buf = (ctypes.c_float * len(flat))(*flat)
            keepalive.append(buf)
            c_guides[i].points = ctypes.cast(buf, ctypes.POINTER(ctypes.c_float))
            c_guides[i].point_count = point_count
            c_guides[i].strength = float(guide.strength)
            c_guides[i].radius = float(guide.radius)
            c_guides[i].mode = guide._mode_code()
            c_guides[i].closed = 1 if guide.closed else 0
        keepalive.append(c_guides)
        c_guidance = _ffi.CyberGuidanceEx()
        c_guidance.guides = (
            ctypes.cast(c_guides, ctypes.POINTER(_ffi.CyberFlowGuideEx)) if guides else None
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

    c_report = None
    if is_zremesher:
        # The ZRemesher entry point, not cyber_remesh with quadMethod=4: it is
        # the only one that carries quality, symmetry and the run report — and
        # the only one that forwards adaptivity, which is where the plain call
        # differed from the CLI for the same request.
        c_zr = (zremesher or ZRemesherParams())._to_c()
        c_report = _ffi.CyberZRemesherReport()
        status = lib.cyber_remesh_zremesher(
            mesh.handle,
            ctypes.byref(c_params),
            ctypes.byref(c_zr),
            ctypes.byref(c_guidance) if c_guidance is not None else None,
            progress_cb,
            cancel_cb,
            warning_cb,
            None,
            ctypes.byref(out_handle),
            ctypes.byref(c_report),
        )
    elif c_guidance is not None:
        status = lib.cyber_remesh_guided_ex(
            mesh.handle,
            ctypes.byref(c_params),
            ctypes.byref(c_guidance),
            progress_cb,
            cancel_cb,
            warning_cb,
            None,
            ctypes.byref(out_handle),
        )
    else:
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
    result.guidance_warnings = list(guidance_warnings)
    if c_report is not None:
        result.zremesher_report = ZRemesherReport._from_c(c_report)
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
    ao_samples: int = 64
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


# ---------------------------------------------------------------------------
# Sculpt handoff bridge (pipeline-bridge)
# ---------------------------------------------------------------------------


class IncompatibleVersionError(CyberError):
    """A versioned file declares a version this engine does not support.

    Distinct from a parse failure: the file is well-formed, the CONTRACT does
    not match. The message names both the version found and the one supported.
    """


class UnsupportedTopologyError(CyberError):
    """The mesh is well-formed but its topology is not what the op needs.

    Distinct from a bad argument: nothing the caller passed is wrong, the
    GEOMETRY is unsuitable — Loop subdivision handed a quad, say — and the fix
    is a mesh edit (:meth:`Mesh.triangulate`) rather than a different value.
    The message names the offending element.
    """


def _raise_status(status: int) -> None:
    """Raises the most specific exception for a non-OK status."""
    if status == _ffi.STATUS_INCOMPATIBLE_VERSION:
        raise IncompatibleVersionError(status, _last_error())
    if status == _ffi.STATUS_UNSUPPORTED_TOPOLOGY:
        raise UnsupportedTopologyError(status, _last_error())
    raise CyberError(status, _last_error())


@dataclass(frozen=True)
class HandoffInfo:
    """What a sculpt handoff declared (mirror of ``CyberHandoffInfo``)."""

    version: Tuple[int, int]
    producer: str
    vertex_count: int
    face_count: int
    has_vertex_colors: bool
    has_vertex_normals: bool
    has_material_mix: bool
    #: Triangles the handoff described that the mesh refused (a repeated vertex
    #: index). Non-zero means the Target carries less geometry than was sent.
    dropped_faces: int = 0

    @staticmethod
    def _from_c(info: "_ffi.CyberHandoffInfo") -> "HandoffInfo":
        producer = info.producer.decode("utf-8", "replace") if info.producer else ""
        return HandoffInfo(
            version=(int(info.version_major), int(info.version_minor)),
            producer=producer,
            vertex_count=int(info.vertex_count),
            face_count=int(info.face_count),
            has_vertex_colors=bool(info.has_vertex_colors),
            has_vertex_normals=bool(info.has_vertex_normals),
            has_material_mix=bool(info.has_material_mix),
            dropped_faces=int(info.dropped_faces),
        )


#: Handoff version this binding supports.
HANDOFF_VERSION = (_ffi.HANDOFF_VERSION_MAJOR, _ffi.HANDOFF_VERSION_MINOR)


class FieldEvaluator:
    """A sampleable field a bake can read instead of raycasting a mesh.

    Subclass and override :meth:`distance`, :meth:`gradient` and
    :meth:`occlusion`. ``distance`` MUST be a Lipschitz-<=1 LOWER bound on the
    true distance to the surface (negative inside): the bake sphere-traces the
    cage ray through it, and a loose bound overshoots and misses thin features.

    The ctypes trampolines are stored on the instance, so keeping a reference
    to the evaluator for the duration of the bake is enough to keep them alive.
    """

    def __init__(self) -> None:
        self._c_distance = _ffi.FIELD_DISTANCE_CB(self._trampoline_distance)
        self._c_gradient = _ffi.FIELD_GRADIENT_CB(self._trampoline_gradient)
        self._c_occlusion = _ffi.FIELD_OCCLUSION_CB(self._trampoline_occlusion)
        self._c_struct = _ffi.CyberFieldEvaluator(
            distance=self._c_distance,
            gradient=self._c_gradient,
            occlusion=self._c_occlusion,
            user=None,
        )

    # -- override these -----------------------------------------------------
    def distance(self, p: Tuple[float, float, float]) -> float:
        raise NotImplementedError

    def gradient(self, p: Tuple[float, float, float]) -> Tuple[float, float, float]:
        raise NotImplementedError

    def occlusion(self, p: Tuple[float, float, float],
                  n: Tuple[float, float, float], radius: float) -> float:
        raise NotImplementedError

    # -- ctypes plumbing ----------------------------------------------------
    # An exception raised inside a ctypes callback cannot propagate across the
    # C frames, so each trampoline degrades to a neutral value rather than
    # letting the bake read uninitialised memory.
    def _trampoline_distance(self, _user, p) -> float:
        try:
            return float(self.distance((p[0], p[1], p[2])))
        except Exception:
            return 0.0

    def _trampoline_gradient(self, _user, p, out) -> None:
        try:
            g = self.gradient((p[0], p[1], p[2]))
        except Exception:
            g = (0.0, 0.0, 1.0)
        out[0], out[1], out[2] = float(g[0]), float(g[1]), float(g[2])

    def _trampoline_occlusion(self, _user, p, n, radius) -> float:
        try:
            return float(self.occlusion((p[0], p[1], p[2]), (n[0], n[1], n[2]), float(radius)))
        except Exception:
            return 1.0


def bake_field(low: "Mesh", bake_map: int, field: FieldEvaluator,
               params: Optional[BakeParams] = None,
               high: Optional["Mesh"] = None) -> Image:
    """Bake ``bake_map`` by sampling ``field`` instead of raycasting a Target.

    ``high`` is optional for NORMAL / AO / CURVATURE / CAVITY — the four maps a
    field can answer on its own; every other map still needs the Target mesh.

    NOTE: every texel calls back into Python, so this is a correctness surface
    rather than a fast path. A C++ or C evaluator runs orders of magnitude
    faster for production bakes.
    """
    if params is None:
        params = BakeParams()
    c_params = params._to_c()
    out = ctypes.c_void_p()
    status = _ffi.get_lib().cyber_bake_field(
        low.handle,
        high.handle if high is not None else None,
        int(bake_map),
        ctypes.byref(c_params),
        ctypes.byref(field._c_struct),
        ctypes.byref(out),
    )
    _check(status)
    if not out.value:
        raise CyberError(_ffi.STATUS_ERROR, _last_error() or "bake produced no image")
    return Image(out.value)


@dataclass(frozen=True)
class ConformReport:
    """Deviation report from :func:`conform`."""

    moved_vertices: int
    max_deviation: float
    rms_deviation: float
    flagged: List[int]


def conform(edit: "Mesh", new_target: "Mesh", threshold: float = 0.0) -> ConformReport:
    """Re-snap ``edit`` onto ``new_target``, preserving its topology exactly.

    The sculpt changed after retopology started: this pulls every vertex back
    onto the new surface, moving nothing else. Vertices whose deviation exceeds
    ``threshold`` (<= 0 disables flagging) come back in
    :attr:`ConformReport.flagged` — the operation still completes; it just
    never stretches silently.
    """
    lib = _ffi.get_lib()
    # Conform mutates the EditMesh, so it must run EXACTLY ONCE: a query pass
    # followed by a fetch pass would report the second (already-conformed) run,
    # which is always zero deviation. The flagged buffer is therefore sized
    # up-front from the vertex count, which bounds it.
    c_stats = _ffi.CyberStatistics()
    capacity = 0
    if lib.cyber_mesh_stats(edit.handle, ctypes.byref(c_stats)) == _ffi.STATUS_OK:
        capacity = int(c_stats.vertex_count)
    buf = (ctypes.c_uint32 * capacity)() if capacity else None
    report = _ffi.CyberConformReport()
    _check(lib.cyber_conform(edit.handle, new_target.handle, float(threshold),
                             ctypes.byref(report), buf, capacity))
    written = min(int(report.flagged_count), capacity)
    flagged: List[int] = [int(buf[i]) for i in range(written)] if buf else []
    return ConformReport(
        moved_vertices=int(report.moved_vertices),
        max_deviation=float(report.max_deviation),
        rms_deviation=float(report.rms_deviation),
        flagged=flagged,
    )


# ---------------------------------------------------------------------------
# Named export presets (mesh-io)
# ---------------------------------------------------------------------------


def builtin_presets() -> List[str]:
    """Names of the shipped built-in export presets, in help-text order."""
    lib = _ffi.get_lib()
    count = int(lib.cyber_export_preset_builtin_count())
    names: List[str] = []
    for i in range(count):
        raw = lib.cyber_export_preset_builtin_name(i)
        if raw:
            names.append(raw.decode("utf-8", "replace"))
    return names


@dataclass(frozen=True)
class PresetMapEntry:
    """One map an export preset requests (mirror of ``CyberExportPresetMap``)."""

    map: str
    color_space: str
    #: Token substituted for ``{map}`` in the preset's naming pattern.
    suffix: str


class ExportPreset:
    """A resolved named export preset — what one target app expects.

    A preset is pure DATA: which maps to bake, how to name the files, what
    colour space and normal-map convention the app reads, which mesh container
    to write. Resolve one with :meth:`resolve`, inspect it, optionally override
    :attr:`resolution`, then hand it to :func:`write_bundle`.

    Owns a C handle; use it as a context manager or call :meth:`close`.
    """

    __slots__ = ("_handle",)

    def __init__(self, handle: int):
        self._handle = handle

    @classmethod
    def resolve(cls, name_or_path: str) -> "ExportPreset":
        """Resolve a built-in preset name, or a path to a preset JSON file.

        A file declaring a schema version this engine does not support raises
        :class:`IncompatibleVersionError` naming both versions — never a
        partially honored preset. A name that is neither built in nor a
        readable file raises :class:`CyberError` listing the built-ins.
        """
        out = ctypes.c_void_p()
        status = _ffi.get_lib().cyber_export_preset_resolve(
            str(name_or_path).encode("utf-8"), ctypes.byref(out)
        )
        if status != _ffi.STATUS_OK:
            _raise_status(status)
        return cls(handle=out.value)

    @property
    def handle(self) -> int:
        if self._handle is None:
            raise CyberError(_ffi.STATUS_ERROR, "preset has been closed")
        return self._handle

    def close(self) -> None:
        if getattr(self, "_handle", None):
            _ffi.get_lib().cyber_export_preset_free(self._handle)
            self._handle = None

    def __enter__(self) -> "ExportPreset":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def __del__(self):  # pragma: no cover - GC timing
        try:
            self.close()
        except Exception:
            pass

    def _info(self) -> "_ffi.CyberExportPresetInfo":
        info = _ffi.CyberExportPresetInfo()
        _check(_ffi.get_lib().cyber_export_preset_info(self.handle, ctypes.byref(info)))
        return info

    @staticmethod
    def _text(raw: Optional[bytes]) -> str:
        return raw.decode("utf-8", "replace") if raw else ""

    @property
    def name(self) -> str:
        return self._text(self._info().name)

    @property
    def schema_version(self) -> int:
        return int(self._info().schema_version)

    @property
    def mesh_format(self) -> str:
        """Mesh container extension without the dot: ``obj``, ``glb``, ..."""
        return self._text(self._info().mesh_format)

    @property
    def texture_format(self) -> str:
        """Texture container extension without the dot: ``png`` or ``exr``."""
        return self._text(self._info().texture_format)

    @property
    def naming_pattern(self) -> str:
        """Tokens: ``{basename}``, ``{map}``, ``{preset}``, ``{ext}``."""
        return self._text(self._info().naming_pattern)

    @property
    def units(self) -> str:
        return self._text(self._info().units)

    @property
    def up_axis(self) -> str:
        return self._text(self._info().up_axis)

    @property
    def normal_green(self) -> str:
        """``"+Y"`` (OpenGL — Blender, Unity, glTF) or ``"-Y"`` (DirectX — Unreal).

        Not cosmetic: reading a map with the wrong convention inverts every
        dent and bump in the shaded result.
        """
        return "+Y" if self._info().normal_green_plus_y else "-Y"

    @property
    def resolution(self) -> int:
        """Texture resolution, in texels per side. Writable."""
        return int(self._info().resolution)

    @resolution.setter
    def resolution(self, value: int) -> None:
        _check(_ffi.get_lib().cyber_export_preset_set_resolution(self.handle, int(value)))

    @property
    def maps(self) -> List[PresetMapEntry]:
        lib = _ffi.get_lib()
        entries: List[PresetMapEntry] = []
        for i in range(int(self._info().map_count)):
            entry = _ffi.CyberExportPresetMap()
            _check(lib.cyber_export_preset_map(self.handle, i, ctypes.byref(entry)))
            entries.append(
                PresetMapEntry(
                    map=self._text(entry.map),
                    color_space=self._text(entry.color_space),
                    suffix=self._text(entry.suffix),
                )
            )
        return entries

    def map_file_name(self, index: int, basename: str) -> str:
        """The file name map ``index`` produces for ``basename``.

        Expanded by the engine, so the token rules never drift from what
        :func:`write_bundle` actually writes.

        :raises CyberError: for an out-of-range index AND for a preset whose
            naming pattern would place the file outside the output directory —
            two different refusals, so the engine's own message is reported
            rather than assuming the first.
        """
        raw = _ffi.get_lib().cyber_export_preset_map_file_name(
            self.handle, int(index), str(basename).encode("utf-8")
        )
        if not raw:
            raise CyberError(_ffi.STATUS_ERROR, _last_error() or "map index out of range")
        return raw.decode("utf-8", "replace")

    def __repr__(self) -> str:
        return "ExportPreset(name={0!r}, mesh_format={1!r}, normal_green={2!r}, maps={3})".format(
            self.name, self.mesh_format, self.normal_green, [m.map for m in self.maps]
        )


@dataclass(frozen=True)
class BundleFile:
    """One file :func:`write_bundle` wrote."""

    path: str
    #: ``"mesh"``, or the preset map name (``"normal"``, ``"ao"``, ...).
    kind: str
    #: Encoding actually written: ``"linear"`` or ``"srgb"``. Empty for the mesh.
    color_space: str
    width: int
    height: int


@dataclass(frozen=True)
class BundleResult:
    """What an export bundle produced."""

    files: List[BundleFile]
    #: Non-fatal notes — a preset/extension mismatch, a map the source could
    #: not feed. Never silently dropped.
    warnings: List[str]
    #: True when the low-poly carried no UVs and the bundle unwrapped it; the
    #: chart count and distortion below are that unwrap's.
    unwrapped: bool
    chart_count: int
    max_angle_distortion: float

    def file(self, kind: str) -> Optional[BundleFile]:
        """The written file of one kind, or None when the bundle has none."""
        for entry in self.files:
            if entry.kind == kind:
                return entry
        return None


def write_bundle(
    low: "Mesh",
    high: "Mesh",
    preset: ExportPreset,
    mesh_path: str,
    basename: str = "",
    cage_distance: Optional[float] = None,
    ao_samples: Optional[int] = None,
    ao_radius: Optional[float] = None,
    progress: Optional[Callable[[float, str], None]] = None,
    cancel: Optional[Callable[[], bool]] = None,
) -> BundleResult:
    """Write ``preset``'s export bundle: the mesh plus one baked map per entry.

    ``low`` IS MODIFIED IN PLACE when it carries no UVs — baking is impossible
    without them, and requiring a pre-unwrap would make presets useless on a
    freshly remeshed mesh. Call ``low.copy()`` first to keep the original.
    ``high`` is the projection source and is never modified.

    ``mesh_path``'s extension wins over the preset's mesh format (an explicit
    output path is the user speaking last); the mismatch comes back in
    :attr:`BundleResult.warnings` rather than being silently resolved.
    ``basename`` (default: the mesh path's stem) fills ``{basename}`` in the
    preset's naming pattern.

    ``progress`` / ``cancel`` behave exactly as in :func:`remesh`; cancelling
    raises :class:`CyberError` with a CANCELLED status and leaves whatever
    files were already written on disk.
    """
    lib = _ffi.get_lib()
    params = _ffi.CyberBundleParams()
    lib.cyber_default_bundle_params(ctypes.byref(params))
    params.mesh_path = str(mesh_path).encode("utf-8")
    params.basename = basename.encode("utf-8") if basename else None
    if cage_distance is not None:
        params.cage_distance = float(cage_distance)
    if ao_samples is not None:
        params.ao_samples = int(ao_samples)
    if ao_radius is not None:
        params.ao_radius = float(ao_radius)

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

    out = ctypes.c_void_p()
    _check(
        lib.cyber_export_bundle_write(
            low.handle,
            high.handle,
            preset.handle,
            ctypes.byref(params),
            progress_cb,
            cancel_cb,
            None,
            ctypes.byref(out),
        )
    )
    try:
        files: List[BundleFile] = []
        for i in range(int(lib.cyber_bundle_result_file_count(out))):
            entry = _ffi.CyberBundleFile()
            _check(lib.cyber_bundle_result_file(out, i, ctypes.byref(entry)))
            files.append(
                BundleFile(
                    path=ExportPreset._text(entry.path),
                    kind=ExportPreset._text(entry.kind),
                    color_space=ExportPreset._text(entry.color_space),
                    width=int(entry.width),
                    height=int(entry.height),
                )
            )
        messages: List[str] = []
        for i in range(int(lib.cyber_bundle_result_warning_count(out))):
            messages.append(ExportPreset._text(lib.cyber_bundle_result_warning(out, i)))
        return BundleResult(
            files=files,
            warnings=messages,
            unwrapped=bool(lib.cyber_bundle_result_unwrapped(out)),
            chart_count=int(lib.cyber_bundle_result_chart_count(out)),
            max_angle_distortion=float(lib.cyber_bundle_result_max_angle_distortion(out)),
        )
    finally:
        lib.cyber_bundle_result_free(out)
