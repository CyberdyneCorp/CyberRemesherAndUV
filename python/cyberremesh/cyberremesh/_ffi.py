"""ctypes binding layer for the CyberRemesher C ABI (`capi` module).

This module mirrors the contract published in ``cyber_capi.h`` and shipped as
``libcyber_capi.so`` / ``.dylib`` / ``cyber_capi.dll``. No C++ type crosses the
boundary: opaque handles are ``void*``, everything else is a plain C scalar,
struct or function pointer, and errors travel as status codes plus a
thread-local ``cyber_last_error()`` string.

Importing this module never dlopens anything. The shared library is located and
loaded lazily on the first :func:`get_lib` call, so ``import cyberremesh`` works
on a machine that has never built the engine (the wrapper simply cannot run an
actual remesh until the library is present).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import stat
import sys
from ctypes import (
    CFUNCTYPE,
    POINTER,
    Structure,
    c_char_p,
    c_float,
    c_int32,
    c_size_t,
    c_uint8,
    c_uint32,
    c_uint64,
    c_void_p,
)
from typing import List, Optional

# ---------------------------------------------------------------------------
# ABI constants
# ---------------------------------------------------------------------------

# cyber_status enum (cyber_capi.h). 0 is success; every other value is an error
# carrying a message retrievable via cyber_last_error().
# These MUST match CyberStatus in capi/include/cyber_capi.h.
STATUS_OK = 0
STATUS_IO_ERROR = 1
STATUS_INVALID_ARGUMENT = 2
STATUS_INVALID_PARAM = 3
STATUS_EMPTY = 4
STATUS_RUNTIME = 5
STATUS_CANCELLED = 6
STATUS_INCOMPATIBLE_VERSION = 7
STATUS_UNSUPPORTED_TOPOLOGY = 8
STATUS_ERROR = STATUS_RUNTIME  # generic-failure alias used by the wrapper

_STATUS_NAMES = {
    STATUS_OK: "OK",
    STATUS_IO_ERROR: "IO_ERROR",
    STATUS_INVALID_ARGUMENT: "INVALID_ARGUMENT",
    STATUS_INVALID_PARAM: "INVALID_PARAM",
    STATUS_EMPTY: "EMPTY",
    STATUS_RUNTIME: "RUNTIME",
    STATUS_CANCELLED: "CANCELLED",
    STATUS_INCOMPATIBLE_VERSION: "INCOMPATIBLE_VERSION",
    STATUS_UNSUPPORTED_TOPOLOGY: "UNSUPPORTED_TOPOLOGY",
}

# cyber_small_patch_policy enum — mirrors cyber::remesh::SmallPatchPolicy.
POLICY_KEEP_LARGEST = 0
POLICY_KEEP_ALL = 1
POLICY_MIN_FACES = 2

# CYBER_INVALID_ID — the sentinel every element-id accessor returns for "none".
INVALID_ID = 0xFFFFFFFF

# CyberLoopSubdivideMode enum — mirrors cyber::retopo::LoopSubdivideMode.
# Persisted by callers, so these values are append-only.
LOOP_SUBDIVIDE_SMOOTH = 0
LOOP_SUBDIVIDE_LINEAR = 1

# cyber_falloff enum — mirrors cyber::retopo::Falloff. Persisted by callers, so
# these values are append-only.
FALLOFF_LINEAR = 0
FALLOFF_SMOOTH = 1
FALLOFF_SHARP = 2
FALLOFF_ROUND = 3

# CyberSubdivisionMode — same topology either way, different vertex positions.
# Persisted by callers, so these values are append-only.
SUBDIV_LINEAR = 0
SUBDIV_CATMULL_CLARK = 1


def status_name(status: int) -> str:
    """Human-readable name for a ``cyber_status`` code."""
    return _STATUS_NAMES.get(status, "UNKNOWN({0})".format(status))


# ---------------------------------------------------------------------------
# ABI structs (must match cyber_capi.h field-for-field)
# ---------------------------------------------------------------------------


class CyberRemeshParams(Structure):
    """Mirror of ``cyber_remesh_params`` — canonical remeshing parameters."""

    # Layout MUST match CyberRemeshParams in capi/include/cyber_capi.h.
    _fields_ = [
        ("target_quad_count", c_int32),
        ("edge_scale", c_float),
        ("sharp_edge_degrees", c_float),
        ("smooth_normal_degrees", c_float),
        ("adaptivity", c_float),
        ("pure_quads", c_int32),  # C int (0/1)
        ("hole_fill_max_boundary", c_int32),
        # 0 = field-aligned, 1 = Instant-Meshes, 2 = integer, 3 = quad-cover,
        # 4 = zremesher
        ("quad_method", c_int32),
    ]


class CyberFlowGuide(Structure):
    """Mirror of ``CyberFlowGuide`` — one polyline flow guide."""

    # Layout MUST match CyberFlowGuide in capi/include/cyber_capi.h.
    _fields_ = [
        ("points", POINTER(c_float)),  # 3 * point_count floats
        ("point_count", c_size_t),
        ("strength", c_float),
        ("radius", c_float),
    ]


class CyberGuidance(Structure):
    """Mirror of ``CyberGuidance`` — flow guides plus painted density."""

    # Layout MUST match CyberGuidance in capi/include/cyber_capi.h.
    _fields_ = [
        ("guides", POINTER(CyberFlowGuide)),
        ("guide_count", c_size_t),
        ("vertex_density", POINTER(c_float)),
        ("vertex_density_count", c_size_t),
        ("face_density", POINTER(c_float)),
        ("face_density_count", c_size_t),
    ]


class CyberStatistics(Structure):
    """Mirror of ``cyber_statistics`` — pipeline result counters."""

    # Layout MUST match CyberStats in capi/include/cyber_capi.h (6 size_t).
    _fields_ = [
        ("vertex_count", c_uint64),
        ("quad_count", c_uint64),
        ("triangle_count", c_uint64),
        ("other_polygon_count", c_uint64),
        ("island_count", c_uint64),
        ("islands_failed", c_uint64),
    ]


class CyberIsotropicParams(Structure):
    """Mirror of ``CyberIsotropicParams`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("target_edge_length", c_float),
        ("iterations", c_int32),
        ("adaptivity", c_float),
        ("smooth_normal_degrees", c_float),
        ("sharp_edge_degrees", c_float),
    ]


class CyberAtlasParams(Structure):
    """Mirror of ``CyberAtlasParams`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("max_chart_angle_degrees", c_float),
        ("pack_margin", c_float),
        ("texture_size", c_int32),
        ("reorient_charts", c_int32),
        ("merge_charts", c_int32),
        ("max_chart_distortion", c_float),
    ]


class CyberUnwrapSeamsParams(Structure):
    """Mirror of ``CyberUnwrapSeamsParams`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("pack_margin", c_float),
        ("texture_size", c_int32),
        ("reorient_charts", c_int32),
    ]


class CyberSeamPathOptions(Structure):
    """Mirror of ``CyberSeamPathOptions`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("flat_weight", c_float),
        ("feature_weight", c_float),
        ("concave_weight", c_float),
        ("convex_weight", c_float),
        ("crease_degrees", c_float),
        ("min_weight", c_float),
    ]


class CyberAtlasResult(Structure):
    """Mirror of ``CyberAtlasResult`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("chart_count", c_int32),
        ("seam_edges", c_uint64),
        ("max_angle_distortion", c_float),
        ("rms_angle_distortion", c_float),
        ("flipped_charts", c_int32),
        ("fallback_charts", c_int32),
        ("packed_area", c_float),
        ("texel_density", c_float),
        ("dropped_charts", c_int32),
        ("packed_box_area", c_float),
    ]


# CyberBakeMap values (must match the enum in cyber_capi.h).
BAKE_NORMAL = 0
BAKE_AO = 1
BAKE_DISPLACEMENT = 2
BAKE_POSITION = 3
BAKE_COLOR = 4
BAKE_CURVATURE = 5
BAKE_CAVITY = 6


class CyberBakeParams(Structure):
    """Mirror of ``CyberBakeParams`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("width", c_int32),
        ("height", c_int32),
        ("cage_distance", c_float),
        ("ao_samples", c_int32),
        ("ao_radius", c_float),
        ("curvature_range", c_float),
    ]


# -- sculpt handoff bridge (pipeline-bridge) --------------------------------

# Handoff version this binding supports; must match CYBER_HANDOFF_VERSION_* in
# capi/include/cyber_capi.h.
HANDOFF_VERSION_MAJOR = 1
HANDOFF_VERSION_MINOR = 0


class CyberHandoffInfo(Structure):
    """Mirror of ``CyberHandoffInfo`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("version_major", c_int32),
        ("version_minor", c_int32),
        ("producer", c_char_p),
        ("vertex_count", c_size_t),
        ("face_count", c_size_t),
        ("has_vertex_colors", c_int32),
        ("has_vertex_normals", c_int32),
        ("has_material_mix", c_int32),
        ("dropped_faces", c_size_t),
    ]


class CyberHandoffBuffers(Structure):
    """Mirror of ``CyberHandoffBuffers`` — the in-memory handoff profile."""

    _fields_ = [
        ("positions", POINTER(c_float)),
        ("normals", POINTER(c_float)),
        ("colors", POINTER(c_float)),
        ("material_mix", POINTER(c_float)),
        ("vertex_count", c_size_t),
        ("indices", POINTER(c_uint32)),
        ("index_count", c_size_t),
        ("version_major", c_int32),
        ("version_minor", c_int32),
        ("producer", c_char_p),
    ]


# -- named export presets (mesh-io) -----------------------------------------

# Preset schema version this binding understands; must match
# CYBER_PRESET_SCHEMA_VERSION in capi/include/cyber_capi.h.
PRESET_SCHEMA_VERSION = 1


class CyberExportPresetInfo(Structure):
    """Mirror of ``CyberExportPresetInfo``."""

    _fields_ = [
        ("name", c_char_p),
        ("schema_version", c_int32),
        ("mesh_format", c_char_p),
        ("texture_format", c_char_p),
        ("naming_pattern", c_char_p),
        ("units", c_char_p),
        ("up_axis", c_char_p),
        ("resolution", c_int32),
        ("normal_green_plus_y", c_int32),
        ("map_count", c_size_t),
    ]


class CyberExportPresetMap(Structure):
    """Mirror of ``CyberExportPresetMap``."""

    _fields_ = [
        ("map", c_char_p),
        ("color_space", c_char_p),
        ("suffix", c_char_p),
    ]


class CyberBundleParams(Structure):
    """Mirror of ``CyberBundleParams``."""

    _fields_ = [
        ("mesh_path", c_char_p),
        ("basename", c_char_p),
        ("cage_distance", c_float),
        ("ao_samples", c_int32),
        ("ao_radius", c_float),
    ]


class CyberBundleFile(Structure):
    """Mirror of ``CyberBundleFile``."""

    _fields_ = [
        ("path", c_char_p),
        ("kind", c_char_p),
        ("color_space", c_char_p),
        ("width", c_int32),
        ("height", c_int32),
    ]


class CyberConformReport(Structure):
    """Mirror of ``CyberConformReport``."""

    _fields_ = [
        ("moved_vertices", c_size_t),
        ("max_deviation", c_float),
        ("rms_deviation", c_float),
        ("flagged_count", c_size_t),
    ]


# ---------------------------------------------------------------------------
# Callback trampolines
# ---------------------------------------------------------------------------

# void (*)(float fraction, const char* stage, void* user)
PROGRESS_CB = CFUNCTYPE(None, c_float, c_char_p, c_void_p)
# int (*)(void* user) — return non-zero to request cancellation.
CANCEL_CB = CFUNCTYPE(c_int32, c_void_p)
# void (*)(const char* message, void* user) — the guidance "loud" channel.
WARNING_CB = CFUNCTYPE(None, c_char_p, c_void_p)

# Field evaluator callbacks (CyberFieldEvaluator). Every instance MUST be kept
# alive by the Python side for as long as the bake runs: ctypes does not own
# the trampolines, and a collected one is a segfault.
FIELD_DISTANCE_CB = CFUNCTYPE(c_float, c_void_p, POINTER(c_float))
FIELD_GRADIENT_CB = CFUNCTYPE(None, c_void_p, POINTER(c_float), POINTER(c_float))
FIELD_OCCLUSION_CB = CFUNCTYPE(c_float, c_void_p, POINTER(c_float), POINTER(c_float), c_float)


class CyberFieldEvaluator(Structure):
    """Mirror of ``CyberFieldEvaluator`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("distance", FIELD_DISTANCE_CB),
        ("gradient", FIELD_GRADIENT_CB),
        ("occlusion", FIELD_OCCLUSION_CB),
        ("user", c_void_p),
    ]


# ---------------------------------------------------------------------------
# Library discovery
# ---------------------------------------------------------------------------

# SOVERSION is the project's major version (capi/CMakeLists.txt), which is what
# an install leaves next to the unversioned developer symlink.
_SOVERSION = 0


def _lib_filenames() -> List[str]:
    """Platform-appropriate shared-library file names, most specific first.

    The build sets ``OUTPUT_NAME cyber_capi`` (capi/CMakeLists.txt), so the real
    artifact is ``libcyber_capi.*``; the versioned soname is listed too because
    an install may ship only that. The ``*_shared`` names are kept as trailing
    fallbacks for anything that bundled the target name verbatim.
    """
    if sys.platform == "darwin":
        return [
            "libcyber_capi.dylib",
            "libcyber_capi.{0}.dylib".format(_SOVERSION),
            "libcyber_capi_shared.dylib",
            "libcyber_capi_shared.so",
        ]
    if sys.platform.startswith("win"):
        return ["cyber_capi.dll", "libcyber_capi.dll",
                "cyber_capi_shared.dll", "libcyber_capi_shared.dll"]
    return [
        "libcyber_capi.so",
        "libcyber_capi.so.{0}".format(_SOVERSION),
        "libcyber_capi_shared.so",
    ]


def _build_tree_dirs(base: str) -> List[str]:
    """The directories inside one build tree that may hold the library.

    ``capi`` is where the module's own output lands; the extra pass over the
    tree's immediate subdirectories covers CMake presets, whose ``binaryDir``
    is ``build/<preset>`` (CMakePresets.json), so the library sits at
    ``build/<preset>/capi``.
    """
    leaves = ("capi", os.path.join("src", "capi"), "lib", "bin")
    dirs = [base] + [os.path.join(base, leaf) for leaf in leaves]
    try:
        entries = sorted(os.listdir(base))
    except OSError:  # no such build tree, or not readable
        return dirs
    for entry in entries:
        preset = os.path.join(base, entry)
        if os.path.isdir(preset):
            dirs.extend(os.path.join(preset, leaf) for leaf in leaves)
    return dirs


def _is_trusted_dir(path: str) -> bool:
    """True when ``path`` exists and no other local user can write into it.

    The build-tree probe hands whatever it finds straight to ``dlopen``, whose
    ELF constructors run before any name or version check, so a directory that
    is world-writable or belongs to somebody else must never be searched.
    Windows has no cheap equivalent of this check, so it is POSIX-only.
    """
    try:
        info = os.stat(path)
    except OSError:  # missing, or not readable
        return False
    if os.name != "posix":
        return True
    if info.st_mode & stat.S_IWOTH:
        return False
    return info.st_uid in (os.getuid(), 0)


def _source_tree_root(start: str) -> Optional[str]:
    """The checkout of THIS project containing ``start``, or ``None``.

    Only an ancestor that actually carries this repo's markers counts, so an
    installed package never picks up a ``build/`` directory that merely happens
    to sit above it (a venv under a shared /tmp, a CI scratch tree).
    """
    root = start
    for _ in range(6):
        root = os.path.dirname(root)
        if not root or root == os.path.dirname(root):
            return None
        markers = (os.path.join(root, "CMakeLists.txt"),
                   os.path.join(root, "capi", "include", "cyber_capi.h"))
        if all(os.path.isfile(m) for m in markers):
            return root if _is_trusted_dir(root) else None
    return None


def _candidate_dirs() -> List[str]:
    """Directories to probe for the shared library, in priority order."""
    dirs: List[str] = []
    here = os.path.abspath(os.path.dirname(__file__))

    # 1. The package directory itself (a wheel bundles the lib alongside).
    dirs.append(here)

    # 2. Conventional build trees of the surrounding source checkout, when the
    #    package really is inside one (at <repo>/python/cyberremesh/cyberremesh)
    #    and that checkout is ours. Each build dir is re-checked, so a
    #    world-writable one inside an otherwise trusted tree is still skipped.
    root = _source_tree_root(here)
    if root is not None:
        for build in ("build", "out", "cmake-build-debug", "cmake-build-release"):
            dirs.extend(d for d in _build_tree_dirs(os.path.join(root, build))
                        if _is_trusted_dir(d))

    return dirs


def find_library_path() -> Optional[str]:
    """Return the resolved path to the shared library, or ``None``.

    Search order:
      1. ``CYBER_CAPI_LIB`` env var (a full path, or a directory containing it).
      2. The package directory (bundled wheel).
      3. Common build directories of the source checkout the package sits in —
         only when an ancestor really is a checkout of this project and is not
         writable by other users. An installed package therefore never picks up
         a build tree that merely happens to sit above it.
      4. The platform loader search path, for a system-installed library — this
         step may return a bare file name rather than a path, which is what
         :func:`ctypes.CDLL` wants anyway.

    This never dlopens the library.
    """
    names = _lib_filenames()

    env = os.environ.get("CYBER_CAPI_LIB")
    if env:
        if os.path.isfile(env):
            return env
        if os.path.isdir(env):
            for name in names:
                candidate = os.path.join(env, name)
                if os.path.isfile(candidate):
                    return candidate

    for directory in _candidate_dirs():
        for name in names:
            candidate = os.path.join(directory, name)
            if os.path.isfile(candidate):
                return os.path.abspath(candidate)

    return ctypes.util.find_library("cyber_capi")


# ---------------------------------------------------------------------------
# argtypes / restypes declaration
# ---------------------------------------------------------------------------


class CyberSoftTransformReport(Structure):
    """Mirror of ``CyberSoftTransformReport`` — weighted transform/relax report."""

    # Layout MUST match CyberSoftTransformReport in capi/include/cyber_capi.h.
    _fields_ = [
        ("moved", c_size_t),
        ("resnapped", c_size_t),
        ("max_snap_distance", c_float),
    ]


def _declare_soft_selection(lib: ctypes.CDLL) -> None:
    """Soft selection (gradient regions, selection ops, weighted transform).

    Auto re-snap is INSIDE ``_transform``/``_relax``: passing a snapper glues the
    moved vertices to the Target within the same call, so no separate snap pass
    is needed (or wanted — it would drag the untouched vertices too).
    """
    # -- region sources ------------------------------------------------------
    lib.cyber_retopo_selection_line.argtypes = [
        c_void_p, POINTER(c_float), POINTER(c_float), POINTER(c_float),
        c_int32, c_float, c_int32,
    ]
    lib.cyber_retopo_selection_line.restype = c_int32

    lib.cyber_retopo_selection_sphere.argtypes = [
        c_void_p, POINTER(c_float), c_float, c_int32,
    ]
    lib.cyber_retopo_selection_sphere.restype = c_int32

    lib.cyber_retopo_selection_paint.argtypes = [
        c_void_p, POINTER(c_float), c_float, c_float, c_int32, c_int32,
    ]
    lib.cyber_retopo_selection_paint.restype = c_int32

    # samples are (x, y, z, pressure) quadruplets — the painted-mode gesture route.
    lib.cyber_retopo_selection_paint_stroke.argtypes = [
        c_void_p, POINTER(c_float), c_size_t, c_float, c_int32, c_int32,
    ]
    lib.cyber_retopo_selection_paint_stroke.restype = c_int32

    # -- selection operations ------------------------------------------------
    lib.cyber_retopo_selection_clear.argtypes = [c_void_p]
    lib.cyber_retopo_selection_clear.restype = c_int32
    lib.cyber_retopo_selection_invert.argtypes = [c_void_p]
    lib.cyber_retopo_selection_invert.restype = c_int32
    lib.cyber_retopo_selection_expand.argtypes = [c_void_p, c_int32]
    lib.cyber_retopo_selection_expand.restype = c_int32
    lib.cyber_retopo_selection_contract.argtypes = [c_void_p, c_int32]
    lib.cyber_retopo_selection_contract.restype = c_int32
    lib.cyber_retopo_selection_smooth.argtypes = [c_void_p, c_int32]
    lib.cyber_retopo_selection_smooth.restype = c_int32

    # -- weight field --------------------------------------------------------
    lib.cyber_retopo_selection_copy_weights.argtypes = [c_void_p, POINTER(c_float), c_size_t]
    lib.cyber_retopo_selection_copy_weights.restype = c_size_t
    lib.cyber_retopo_selection_set_weights.argtypes = [c_void_p, POINTER(c_float), c_size_t]
    lib.cyber_retopo_selection_set_weights.restype = c_int32

    # -- named slots ---------------------------------------------------------
    lib.cyber_retopo_selection_save.argtypes = [c_void_p, c_char_p]
    lib.cyber_retopo_selection_save.restype = c_int32
    lib.cyber_retopo_selection_load.argtypes = [c_void_p, c_char_p]
    lib.cyber_retopo_selection_load.restype = c_int32
    lib.cyber_retopo_selection_slot_count.argtypes = [c_void_p]
    lib.cyber_retopo_selection_slot_count.restype = c_size_t
    lib.cyber_retopo_selection_slot_name.argtypes = [c_void_p, c_size_t]
    lib.cyber_retopo_selection_slot_name.restype = c_char_p

    # -- weighted transform / relax -----------------------------------------
    lib.cyber_retopo_selection_transform.argtypes = [
        c_void_p, POINTER(c_float), c_void_p, c_float, POINTER(CyberSoftTransformReport),
    ]
    lib.cyber_retopo_selection_transform.restype = c_int32
    lib.cyber_retopo_selection_transform_pinned.argtypes = [
        c_void_p, POINTER(c_float), POINTER(c_uint32), c_size_t, c_void_p, c_float,
        POINTER(CyberSoftTransformReport),
    ]
    lib.cyber_retopo_selection_transform_pinned.restype = c_int32
    lib.cyber_retopo_selection_relax.argtypes = [
        c_void_p, c_float, c_int32, POINTER(c_uint32), c_size_t, c_void_p, c_float,
        POINTER(CyberSoftTransformReport),
    ]
    lib.cyber_retopo_selection_relax.restype = c_int32


def _declare_retopo_ops(lib: ctypes.CDLL) -> None:
    """Whole-mesh and local retopology edits (cyber_capi.h "EditMesh batch
    commands" plus the gesture ops that need no stroke geometry).

    ELEMENT-ID STABILITY, copied from the header because callers key
    annotations on these ids: ``snap_all`` moves positions only, so every
    vertex/edge/face id survives; ``triangulate`` keeps vertex and edge ids and
    the ids of the faces it splits, but each n-gon gains NEW face ids for its
    extra triangles; the topology ops below (delete_faces, dissolve_edges,
    insert_loop, merge_vertices, rotate_edge) retire the elements they consume,
    so ids collected before the call must be re-read after it.

    ``snapper`` is a plain ``c_void_p`` here, the same way
    :func:`_declare_soft_selection` passes it: it is an opaque handle and NULL
    means "no Target snapping".
    """
    # CyberStatus cyber_retopo_relax(CyberMesh*, const float[3], float, float,
    #                                int, int, const uint32_t*, size_t,
    #                                const CyberSnapper*)
    # radius <= 0 relaxes the whole mesh, which is why there is no separate
    # relax-all entry point in the ABI.
    lib.cyber_retopo_relax.argtypes = [
        c_void_p, POINTER(c_float), c_float, c_float, c_int32, c_int32,
        POINTER(c_uint32), c_size_t, c_void_p,
    ]
    lib.cyber_retopo_relax.restype = c_int32

    # CyberStatus cyber_retopo_snap_all(CyberMesh*, const CyberSnapper*,
    #                                   const uint32_t*, size_t, size_t*, float*)
    lib.cyber_retopo_snap_all.argtypes = [
        c_void_p, c_void_p, POINTER(c_uint32), c_size_t, POINTER(c_size_t),
        POINTER(c_float),
    ]
    lib.cyber_retopo_snap_all.restype = c_int32

    # CyberStatus cyber_retopo_triangulate(CyberMesh*, size_t*)
    lib.cyber_retopo_triangulate.argtypes = [c_void_p, POINTER(c_size_t)]
    lib.cyber_retopo_triangulate.restype = c_int32

    # CyberStatus cyber_retopo_delete_faces(CyberMesh*, const uint32_t*, size_t,
    #                                       size_t*)
    lib.cyber_retopo_delete_faces.argtypes = [
        c_void_p, POINTER(c_uint32), c_size_t, POINTER(c_size_t),
    ]
    lib.cyber_retopo_delete_faces.restype = c_int32

    # CyberStatus cyber_retopo_dissolve_edges(CyberMesh*, const uint32_t*,
    #                                         size_t, size_t*)
    lib.cyber_retopo_dissolve_edges.argtypes = [
        c_void_p, POINTER(c_uint32), c_size_t, POINTER(c_size_t),
    ]
    lib.cyber_retopo_dissolve_edges.restype = c_int32

    # CyberStatus cyber_retopo_insert_loop(CyberMesh*, uint32_t, float, size_t*)
    lib.cyber_retopo_insert_loop.argtypes = [
        c_void_p, c_uint32, c_float, POINTER(c_size_t),
    ]
    lib.cyber_retopo_insert_loop.restype = c_int32

    # CyberStatus cyber_retopo_merge_vertices(CyberMesh*, uint32_t, uint32_t, int)
    lib.cyber_retopo_merge_vertices.argtypes = [c_void_p, c_uint32, c_uint32, c_int32]
    lib.cyber_retopo_merge_vertices.restype = c_int32

    # CyberStatus cyber_retopo_rotate_edge(CyberMesh*, uint32_t)
    lib.cyber_retopo_rotate_edge.argtypes = [c_void_p, c_uint32]
    lib.cyber_retopo_rotate_edge.restype = c_int32


def _declare_seam_path(lib: ctypes.CDLL) -> None:
    """Auto-routed seam paths (the UV Path tool).

    Handle-based like the snapper: a ``CyberSeamSet`` holds the committed seam
    edges and a ``CyberSeamPath`` holds the editable waypoints. The size-query
    readers follow the ``copy_positions`` convention (total returned, at most
    ``max`` filled).
    """
    lib.cyber_default_seam_path_options.argtypes = [POINTER(CyberSeamPathOptions)]
    lib.cyber_default_seam_path_options.restype = None
    lib.cyber_mesh_edge_signed_dihedral.argtypes = [c_void_p, c_uint32]
    lib.cyber_mesh_edge_signed_dihedral.restype = c_float

    # -- seam set ------------------------------------------------------------
    lib.cyber_seam_set_create.argtypes = [POINTER(c_void_p)]
    lib.cyber_seam_set_create.restype = c_int32
    lib.cyber_seam_set_free.argtypes = [c_void_p]
    lib.cyber_seam_set_free.restype = None
    lib.cyber_seam_set_count.argtypes = [c_void_p]
    lib.cyber_seam_set_count.restype = c_size_t
    lib.cyber_seam_set_is_seam.argtypes = [c_void_p, c_uint32]
    lib.cyber_seam_set_is_seam.restype = c_int32
    lib.cyber_seam_set_edges.argtypes = [c_void_p, POINTER(c_uint32), c_size_t]
    lib.cyber_seam_set_edges.restype = c_size_t
    lib.cyber_seam_set_mark.argtypes = [c_void_p, c_uint32]
    lib.cyber_seam_set_mark.restype = c_int32
    lib.cyber_seam_set_erase.argtypes = [c_void_p, c_uint32]
    lib.cyber_seam_set_erase.restype = c_int32

    # -- pending path --------------------------------------------------------
    lib.cyber_seam_path_create.argtypes = [
        c_void_p, POINTER(CyberSeamPathOptions), POINTER(c_void_p),
    ]
    lib.cyber_seam_path_create.restype = c_int32
    lib.cyber_seam_path_free.argtypes = [c_void_p]
    lib.cyber_seam_path_free.restype = None

    lib.cyber_seam_path_add_waypoint.argtypes = [c_void_p, c_uint32]
    lib.cyber_seam_path_add_waypoint.restype = c_int32
    lib.cyber_seam_path_move_waypoint.argtypes = [c_void_p, c_size_t, c_uint32]
    lib.cyber_seam_path_move_waypoint.restype = c_int32
    lib.cyber_seam_path_move_waypoint_to.argtypes = [
        c_void_p, c_size_t, POINTER(c_float), c_float,
    ]
    lib.cyber_seam_path_move_waypoint_to.restype = c_int32
    lib.cyber_seam_path_remove_waypoint.argtypes = [c_void_p, c_size_t]
    lib.cyber_seam_path_remove_waypoint.restype = c_int32
    lib.cyber_seam_path_clear.argtypes = [c_void_p]
    lib.cyber_seam_path_clear.restype = None

    lib.cyber_seam_path_waypoint_count.argtypes = [c_void_p]
    lib.cyber_seam_path_waypoint_count.restype = c_size_t
    lib.cyber_seam_path_waypoints.argtypes = [c_void_p, POINTER(c_uint32), c_size_t]
    lib.cyber_seam_path_waypoints.restype = c_size_t
    lib.cyber_seam_path_segment_count.argtypes = [c_void_p]
    lib.cyber_seam_path_segment_count.restype = c_size_t
    lib.cyber_seam_path_segment.argtypes = [c_void_p, c_size_t, POINTER(c_uint32), c_size_t]
    lib.cyber_seam_path_segment.restype = c_size_t
    lib.cyber_seam_path_segment_revision.argtypes = [c_void_p, c_size_t]
    lib.cyber_seam_path_segment_revision.restype = c_uint64
    lib.cyber_seam_path_is_routed.argtypes = [c_void_p]
    lib.cyber_seam_path_is_routed.restype = c_int32
    lib.cyber_seam_path_vertices.argtypes = [c_void_p, POINTER(c_uint32), c_size_t]
    lib.cyber_seam_path_vertices.restype = c_size_t
    lib.cyber_seam_path_edges.argtypes = [c_void_p, POINTER(c_uint32), c_size_t]
    lib.cyber_seam_path_edges.restype = c_size_t

    lib.cyber_seam_path_commit.argtypes = [c_void_p, c_void_p, POINTER(c_uint32), c_size_t]
    lib.cyber_seam_path_commit.restype = c_size_t
    lib.cyber_seam_path_resume_marker.argtypes = [c_void_p]
    lib.cyber_seam_path_resume_marker.restype = c_uint32
    lib.cyber_seam_path_drop_resume_marker.argtypes = [c_void_p]
    lib.cyber_seam_path_drop_resume_marker.restype = None


def _declare(lib: ctypes.CDLL) -> None:
    """Attach argtypes/restypes to every ``cyber_*`` entry point."""

    # -- runtime info --------------------------------------------------------
    # void cyber_version(int* major, int* minor, int* patch)
    lib.cyber_version.argtypes = [POINTER(c_int32), POINTER(c_int32), POINTER(c_int32)]
    lib.cyber_version.restype = None

    lib.cyber_last_error.argtypes = []
    lib.cyber_last_error.restype = c_char_p

    # -- mesh lifetime -------------------------------------------------------
    lib.cyber_mesh_create.argtypes = []
    lib.cyber_mesh_create.restype = c_void_p

    lib.cyber_mesh_destroy.argtypes = [c_void_p]
    lib.cyber_mesh_destroy.restype = None

    # CyberStatus cyber_mesh_clone(const CyberMesh*, CyberMesh** out)
    lib.cyber_mesh_clone.argtypes = [c_void_p, POINTER(c_void_p)]
    lib.cyber_mesh_clone.restype = c_int32

    # -- mesh I/O ------------------------------------------------------------
    # Both dispatch on the file extension (.obj/.ply/.stl/.gltf/.glb, plus .fbx
    # on load — FBX is import-only). The `_obj` spellings are the historical
    # aliases the C ABI keeps for older callers.
    # CyberStatus cyber_mesh_load(const char* path, CyberMesh** out)
    for _loader in ("cyber_mesh_load", "cyber_mesh_load_obj"):
        getattr(lib, _loader).argtypes = [c_char_p, POINTER(c_void_p)]
        getattr(lib, _loader).restype = c_int32

    for _saver in ("cyber_mesh_save", "cyber_mesh_save_obj"):
        getattr(lib, _saver).argtypes = [c_void_p, c_char_p]
        getattr(lib, _saver).restype = c_int32

    # CyberStatus cyber_mesh_stats(const CyberMesh*, CyberStats* out)
    lib.cyber_mesh_stats.argtypes = [c_void_p, POINTER(CyberStatistics)]
    lib.cyber_mesh_stats.restype = c_int32

    # Adaptive isotropic (triangle) remesh in place — the density control the
    # bindings previously could not reach at all.
    # void cyber_default_isotropic_params(CyberIsotropicParams*)
    lib.cyber_default_isotropic_params.argtypes = [POINTER(CyberIsotropicParams)]
    lib.cyber_default_isotropic_params.restype = None
    # CyberStatus cyber_mesh_isotropic_remesh(CyberMesh*, const CyberIsotropicParams*, size_t*)
    lib.cyber_mesh_isotropic_remesh.argtypes = [
        c_void_p, POINTER(CyberIsotropicParams), POINTER(c_size_t)
    ]
    lib.cyber_mesh_isotropic_remesh.restype = c_int32

    lib.cyber_default_atlas_params.argtypes = [POINTER(CyberAtlasParams)]
    lib.cyber_default_atlas_params.restype = None
    lib.cyber_uv_atlas.argtypes = [c_void_p, POINTER(CyberAtlasParams), POINTER(CyberAtlasResult)]
    lib.cyber_uv_atlas.restype = c_int32

    # Unwrap along a caller-built seam set — the counterpart to cyber_uv_atlas,
    # which computes its own cuts and ignores whatever the caller marked.
    lib.cyber_default_unwrap_seams_params.argtypes = [POINTER(CyberUnwrapSeamsParams)]
    lib.cyber_default_unwrap_seams_params.restype = None
    lib.cyber_uv_unwrap_seams.argtypes = [
        c_void_p, c_void_p, POINTER(CyberUnwrapSeamsParams), POINTER(CyberAtlasResult)
    ]
    lib.cyber_uv_unwrap_seams.restype = c_int32

    # CyberStatus cyber_uv_stitch_seams(CyberMesh*, CyberSeamSet*, const uint32_t*, size_t)
    lib.cyber_uv_stitch_seams.argtypes = [c_void_p, c_void_p, POINTER(c_uint32), c_size_t]
    lib.cyber_uv_stitch_seams.restype = c_int32

    # -- mesh queries --------------------------------------------------------
    lib.cyber_mesh_vertex_count.argtypes = [c_void_p]
    lib.cyber_mesh_vertex_count.restype = c_size_t

    lib.cyber_mesh_face_count.argtypes = [c_void_p]
    lib.cyber_mesh_face_count.restype = c_size_t

    # Copies up to `capacity` floats of packed xyz positions into `out`;
    # returns the total float count available (vertex_count * 3).
    lib.cyber_mesh_copy_positions.argtypes = [c_void_p, POINTER(c_float), c_size_t]
    lib.cyber_mesh_copy_positions.restype = c_size_t

    # CyberStatus cyber_mesh_set_positions(CyberMesh*, const float*, size_t)
    lib.cyber_mesh_set_positions.argtypes = [c_void_p, POINTER(c_float), c_size_t]
    lib.cyber_mesh_set_positions.restype = c_int32

    # Edge ids are dense in [0, edge_count); without this a caller has no way to
    # enumerate the edges it wants to mark as seams.
    lib.cyber_mesh_edge_count.argtypes = [c_void_p]
    lib.cyber_mesh_edge_count.restype = c_size_t

    # int cyber_mesh_edge_endpoints(const CyberMesh*, uint32_t, uint32_t out[2])
    lib.cyber_mesh_edge_endpoints.argtypes = [c_void_p, c_uint32, POINTER(c_uint32)]
    lib.cyber_mesh_edge_endpoints.restype = c_int32

    # int cyber_mesh_vertex_position(const CyberMesh*, uint32_t, float out[3])
    lib.cyber_mesh_vertex_position.argtypes = [c_void_p, c_uint32, POINTER(c_float)]
    lib.cyber_mesh_vertex_position.restype = c_int32

    # -- mesh editing --------------------------------------------------------
    # Linear (Catmull-Clark topology, no smoothing) subdivision in place; the
    # optional snapper reprojects the result onto a target surface.
    # CyberStatus cyber_retopo_subdivide(CyberMesh*, const CyberSnapper*, size_t*)
    lib.cyber_retopo_subdivide.argtypes = [c_void_p, c_void_p, POINTER(c_size_t)]
    lib.cyber_retopo_subdivide.restype = c_int32

    # Same, with a SUBDIV_* mode; the mode-less call above is its linear case.
    # CyberStatus cyber_retopo_subdivide_ex(CyberMesh*, const CyberSnapper*,
    #                                       CyberSubdivisionMode, size_t*)
    lib.cyber_retopo_subdivide_ex.argtypes = [c_void_p, c_void_p, c_int32, POINTER(c_size_t)]
    lib.cyber_retopo_subdivide_ex.restype = c_int32

    # Loop subdivision: triangles in, triangles out (1 -> 4). The mode decides
    # whether vertices are repositioned; it is never inferred.
    # CyberStatus cyber_retopo_loop_subdivide(CyberMesh*, CyberLoopSubdivideMode,
    #                                         const CyberSnapper*, size_t*)
    lib.cyber_retopo_loop_subdivide.argtypes = [c_void_p, c_int32, c_void_p, POINTER(c_size_t)]
    lib.cyber_retopo_loop_subdivide.restype = c_int32

    # Fan-triangulation, the explicit opt-in Loop subdivision points a caller
    # at when it refuses an n-gon.
    # CyberStatus cyber_retopo_triangulate(CyberMesh*, size_t*)
    lib.cyber_retopo_triangulate.argtypes = [c_void_p, POINTER(c_size_t)]
    lib.cyber_retopo_triangulate.restype = c_int32

    # CyberStatus cyber_snapper_create(const CyberMesh* target, CyberSnapper** out)
    lib.cyber_snapper_create.argtypes = [c_void_p, POINTER(c_void_p)]
    lib.cyber_snapper_create.restype = c_int32
    lib.cyber_snapper_free.argtypes = [c_void_p]
    lib.cyber_snapper_free.restype = None

    # -- parameters ----------------------------------------------------------
    lib.cyber_remesh_params_default.argtypes = [POINTER(CyberRemeshParams)]
    lib.cyber_remesh_params_default.restype = None

    # -- pipeline ------------------------------------------------------------
    # CyberStatus cyber_remesh(const CyberMesh* in, const CyberRemeshParams*,
    #                          CyberProgressCb progress, CyberCancelCb cancel,
    #                          void* user, CyberMesh** out)
    lib.cyber_remesh.argtypes = [
        c_void_p,
        POINTER(CyberRemeshParams),
        PROGRESS_CB,
        CANCEL_CB,
        c_void_p,
        POINTER(c_void_p),
    ]
    lib.cyber_remesh.restype = c_int32

    # CyberStatus cyber_remesh_guided(const CyberMesh* in, const CyberRemeshParams*,
    #                                 const CyberGuidance*, CyberProgressCb,
    #                                 CyberCancelCb, CyberWarningCb, void* user,
    #                                 CyberMesh** out)
    lib.cyber_remesh_guided.argtypes = [
        c_void_p,
        POINTER(CyberRemeshParams),
        POINTER(CyberGuidance),
        PROGRESS_CB,
        CANCEL_CB,
        WARNING_CB,
        c_void_p,
        POINTER(c_void_p),
    ]
    lib.cyber_remesh_guided.restype = c_int32

    # -- surface baking ------------------------------------------------------
    lib.cyber_default_bake_params.argtypes = [POINTER(CyberBakeParams)]
    lib.cyber_default_bake_params.restype = None
    # CyberStatus cyber_bake(const CyberMesh* low, const CyberMesh* high,
    #                        CyberBakeMap map, const CyberBakeParams*, CyberImage** out)
    lib.cyber_bake.argtypes = [
        c_void_p,
        c_void_p,
        c_int32,
        POINTER(CyberBakeParams),
        POINTER(c_void_p),
    ]
    lib.cyber_bake.restype = c_int32
    lib.cyber_image_free.argtypes = [c_void_p]
    lib.cyber_image_free.restype = None
    for accessor in ("cyber_image_width", "cyber_image_height", "cyber_image_channels"):
        getattr(lib, accessor).argtypes = [c_void_p]
        getattr(lib, accessor).restype = c_int32
    lib.cyber_image_copy_pixels.argtypes = [c_void_p, POINTER(c_float), c_size_t]
    lib.cyber_image_copy_pixels.restype = c_size_t
    lib.cyber_image_save_png.argtypes = [c_void_p, c_char_p]
    lib.cyber_image_save_png.restype = c_int32

    # -- Target snapper ------------------------------------------------------
    # CyberStatus cyber_snapper_create(const CyberMesh* target, CyberSnapper** out)
    lib.cyber_snapper_create.argtypes = [c_void_p, POINTER(c_void_p)]
    lib.cyber_snapper_create.restype = c_int32
    lib.cyber_snapper_free.argtypes = [c_void_p]
    lib.cyber_snapper_free.restype = None

    _declare_soft_selection(lib)
    _declare_retopo_ops(lib)
    _declare_document(lib)
    _declare_seam_path(lib)
    _declare_bridge(lib)
    _declare_export_presets(lib)


def _declare_document(lib: ctypes.CDLL) -> None:
    """The persisted document handle (application-shell spec).

    The seam that carries named soft-selection slots between a mesh handle
    (session state) and a saved file. Byte readers follow the
    ``copy_positions`` convention (total returned, at most ``capacity``
    filled).
    """
    lib.cyber_document_create.argtypes = []
    lib.cyber_document_create.restype = c_void_p
    lib.cyber_document_free.argtypes = [c_void_p]
    lib.cyber_document_free.restype = None
    lib.cyber_document_set_target.argtypes = [c_void_p, c_void_p]
    lib.cyber_document_set_target.restype = c_int32
    lib.cyber_document_set_edit_mesh.argtypes = [c_void_p, c_void_p]
    lib.cyber_document_set_edit_mesh.restype = c_int32
    lib.cyber_document_target.argtypes = [c_void_p]
    lib.cyber_document_target.restype = c_void_p
    lib.cyber_document_edit_mesh.argtypes = [c_void_p]
    lib.cyber_document_edit_mesh.restype = c_void_p
    lib.cyber_document_slot_count.argtypes = [c_void_p]
    lib.cyber_document_slot_count.restype = c_size_t
    lib.cyber_document_slot_name.argtypes = [c_void_p, c_size_t]
    lib.cyber_document_slot_name.restype = c_char_p
    lib.cyber_document_slot_weights.argtypes = [c_void_p, c_char_p, POINTER(c_float), c_size_t]
    lib.cyber_document_slot_weights.restype = c_size_t
    lib.cyber_document_save.argtypes = [c_void_p, POINTER(c_uint8), c_size_t]
    lib.cyber_document_save.restype = c_size_t
    lib.cyber_document_load.argtypes = [POINTER(c_uint8), c_size_t]
    lib.cyber_document_load.restype = c_void_p
    lib.cyber_document_save_file.argtypes = [c_void_p, c_char_p]
    lib.cyber_document_save_file.restype = c_int32
    lib.cyber_document_load_file.argtypes = [c_char_p]
    lib.cyber_document_load_file.restype = c_void_p


def _declare_bridge(lib: ctypes.CDLL) -> None:
    """Sculpt handoff ingest, field-sampled baking and conform."""
    # CyberStatus cyber_handoff_open(const char*, CyberMesh**, CyberHandoffInfo*)
    lib.cyber_handoff_open.argtypes = [c_char_p, POINTER(c_void_p), POINTER(CyberHandoffInfo)]
    lib.cyber_handoff_open.restype = c_int32
    lib.cyber_handoff_open_buffers.argtypes = [
        POINTER(CyberHandoffBuffers),
        POINTER(c_void_p),
        POINTER(CyberHandoffInfo),
    ]
    lib.cyber_handoff_open_buffers.restype = c_int32
    # CyberStatus cyber_bake_field(low, high, map, params, field, out)
    lib.cyber_bake_field.argtypes = [
        c_void_p,
        c_void_p,
        c_int32,
        POINTER(CyberBakeParams),
        POINTER(CyberFieldEvaluator),
        POINTER(c_void_p),
    ]
    lib.cyber_bake_field.restype = c_int32
    # CyberStatus cyber_conform(edit, new_target, threshold, report, out_flagged, max)
    lib.cyber_conform.argtypes = [
        c_void_p,
        c_void_p,
        c_float,
        POINTER(CyberConformReport),
        POINTER(c_uint32),
        c_size_t,
    ]
    lib.cyber_conform.restype = c_int32


def _declare_export_presets(lib: ctypes.CDLL) -> None:
    """Named export presets and the bundle writer."""
    lib.cyber_export_preset_builtin_count.argtypes = []
    lib.cyber_export_preset_builtin_count.restype = c_size_t
    lib.cyber_export_preset_builtin_name.argtypes = [c_size_t]
    lib.cyber_export_preset_builtin_name.restype = c_char_p
    lib.cyber_export_preset_resolve.argtypes = [c_char_p, POINTER(c_void_p)]
    lib.cyber_export_preset_resolve.restype = c_int32
    lib.cyber_export_preset_free.argtypes = [c_void_p]
    lib.cyber_export_preset_free.restype = None
    lib.cyber_export_preset_info.argtypes = [c_void_p, POINTER(CyberExportPresetInfo)]
    lib.cyber_export_preset_info.restype = c_int32
    lib.cyber_export_preset_map.argtypes = [c_void_p, c_size_t, POINTER(CyberExportPresetMap)]
    lib.cyber_export_preset_map.restype = c_int32
    lib.cyber_export_preset_map_file_name.argtypes = [c_void_p, c_size_t, c_char_p]
    lib.cyber_export_preset_map_file_name.restype = c_char_p
    lib.cyber_export_preset_set_resolution.argtypes = [c_void_p, c_int32]
    lib.cyber_export_preset_set_resolution.restype = c_int32

    lib.cyber_default_bundle_params.argtypes = [POINTER(CyberBundleParams)]
    lib.cyber_default_bundle_params.restype = None
    # CyberStatus cyber_export_bundle_write(low, high, preset, params,
    #                                       progress, cancel, user, out)
    lib.cyber_export_bundle_write.argtypes = [
        c_void_p,
        c_void_p,
        c_void_p,
        POINTER(CyberBundleParams),
        PROGRESS_CB,
        CANCEL_CB,
        c_void_p,
        POINTER(c_void_p),
    ]
    lib.cyber_export_bundle_write.restype = c_int32
    lib.cyber_bundle_result_free.argtypes = [c_void_p]
    lib.cyber_bundle_result_free.restype = None
    lib.cyber_bundle_result_file_count.argtypes = [c_void_p]
    lib.cyber_bundle_result_file_count.restype = c_size_t
    lib.cyber_bundle_result_file.argtypes = [c_void_p, c_size_t, POINTER(CyberBundleFile)]
    lib.cyber_bundle_result_file.restype = c_int32
    lib.cyber_bundle_result_warning_count.argtypes = [c_void_p]
    lib.cyber_bundle_result_warning_count.restype = c_size_t
    lib.cyber_bundle_result_warning.argtypes = [c_void_p, c_size_t]
    lib.cyber_bundle_result_warning.restype = c_char_p
    lib.cyber_bundle_result_unwrapped.argtypes = [c_void_p]
    lib.cyber_bundle_result_unwrapped.restype = c_int32
    lib.cyber_bundle_result_chart_count.argtypes = [c_void_p]
    lib.cyber_bundle_result_chart_count.restype = c_int32
    lib.cyber_bundle_result_max_angle_distortion.argtypes = [c_void_p]
    lib.cyber_bundle_result_max_angle_distortion.restype = c_float


# ---------------------------------------------------------------------------
# Lazy singleton loader
# ---------------------------------------------------------------------------

_LIB: Optional[ctypes.CDLL] = None


class LibraryNotFound(RuntimeError):
    """Raised when the C ABI shared library cannot be located or loaded."""


def get_lib() -> ctypes.CDLL:
    """Load (once) and return the declared C ABI library.

    Raises :class:`LibraryNotFound` if the shared object cannot be located or
    fails to load. Safe to call repeatedly; the handle is cached.
    """
    global _LIB
    if _LIB is not None:
        return _LIB

    path = find_library_path()
    if path is None:
        raise LibraryNotFound(
            "Could not locate the CyberRemesher C ABI library "
            "({0}). Set CYBER_CAPI_LIB to its path or build the `capi` "
            "module.".format(" / ".join(_lib_filenames()))
        )

    try:
        lib = ctypes.CDLL(path)
    except OSError as exc:  # pragma: no cover - platform/link failure
        raise LibraryNotFound("Failed to load {0}: {1}".format(path, exc)) from exc

    _declare(lib)
    _LIB = lib
    return _LIB


def is_available() -> bool:
    """True if the shared library can be located and loaded right now."""
    try:
        get_lib()
        return True
    except LibraryNotFound:
        return False
