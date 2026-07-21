"""ctypes binding layer for the CyberRemesher C ABI (`capi` module).

This module mirrors the contract published in ``cyber_capi.h`` and shipped as
``libcyber_capi_shared.so`` / ``.dylib`` / ``cyber_capi_shared.dll``. No C++
type crosses the boundary: opaque handles are ``void*``, everything else is a
plain C scalar, struct or function pointer, and errors travel as status codes
plus a thread-local ``cyber_last_error()`` string.

Importing this module never dlopens anything. The shared library is located and
loaded lazily on the first :func:`get_lib` call, so ``import cyberremesh`` works
on a machine that has never built the engine (the wrapper simply cannot run an
actual remesh until the library is present).
"""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    CFUNCTYPE,
    POINTER,
    Structure,
    c_char_p,
    c_float,
    c_int32,
    c_size_t,
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
STATUS_ERROR = STATUS_RUNTIME  # generic-failure alias used by the wrapper

_STATUS_NAMES = {
    STATUS_OK: "OK",
    STATUS_IO_ERROR: "IO_ERROR",
    STATUS_INVALID_ARGUMENT: "INVALID_ARGUMENT",
    STATUS_INVALID_PARAM: "INVALID_PARAM",
    STATUS_EMPTY: "EMPTY",
    STATUS_RUNTIME: "RUNTIME",
    STATUS_CANCELLED: "CANCELLED",
}

# cyber_small_patch_policy enum — mirrors cyber::remesh::SmallPatchPolicy.
POLICY_KEEP_LARGEST = 0
POLICY_KEEP_ALL = 1
POLICY_MIN_FACES = 2


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
        ("quad_method", c_int32),  # 0 = field-aligned, 1 = Instant-Meshes extractor
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


class CyberAtlasParams(Structure):
    """Mirror of ``CyberAtlasParams`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("max_chart_angle_degrees", c_float),
        ("pack_margin", c_float),
        ("texture_size", c_int32),
        ("reorient_charts", c_int32),
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
    ]


# CyberBakeMap values (must match the enum in cyber_capi.h).
BAKE_NORMAL = 0
BAKE_AO = 1
BAKE_DISPLACEMENT = 2
BAKE_POSITION = 3
BAKE_COLOR = 4


class CyberBakeParams(Structure):
    """Mirror of ``CyberBakeParams`` in capi/include/cyber_capi.h."""

    _fields_ = [
        ("width", c_int32),
        ("height", c_int32),
        ("cage_distance", c_float),
        ("ao_samples", c_int32),
        ("ao_radius", c_float),
    ]


# ---------------------------------------------------------------------------
# Callback trampolines
# ---------------------------------------------------------------------------

# void (*)(float fraction, const char* stage, void* user)
PROGRESS_CB = CFUNCTYPE(None, c_float, c_char_p, c_void_p)
# int (*)(void* user) — return non-zero to request cancellation.
CANCEL_CB = CFUNCTYPE(c_int32, c_void_p)


# ---------------------------------------------------------------------------
# Library discovery
# ---------------------------------------------------------------------------


def _lib_filenames() -> List[str]:
    """Platform-appropriate shared-library file names, most specific first."""
    if sys.platform == "darwin":
        return ["libcyber_capi_shared.dylib", "libcyber_capi_shared.so"]
    if sys.platform.startswith("win"):
        return ["cyber_capi_shared.dll", "libcyber_capi_shared.dll"]
    return ["libcyber_capi_shared.so"]


def _candidate_dirs() -> List[str]:
    """Directories to probe for the shared library, in priority order."""
    dirs: List[str] = []
    here = os.path.abspath(os.path.dirname(__file__))

    # 1. The package directory itself (a wheel bundles the lib alongside).
    dirs.append(here)

    # 2. Walk up toward the repo root, checking conventional build trees. The
    #    package lives at <repo>/python/cyberremesh/cyberremesh, so the repo
    #    root is three levels up in a source checkout.
    root = here
    for _ in range(6):
        root = os.path.dirname(root)
        if not root or root == os.path.dirname(root):
            break
        for build in ("build", "out", "cmake-build-debug", "cmake-build-release"):
            base = os.path.join(root, build)
            dirs.append(base)
            # capi module output typically lands under its source subtree.
            dirs.append(os.path.join(base, "src", "capi"))
            dirs.append(os.path.join(base, "lib"))
            dirs.append(os.path.join(base, "bin"))

    return dirs


def find_library_path() -> Optional[str]:
    """Return the resolved path to the shared library, or ``None``.

    Search order:
      1. ``CYBER_CAPI_LIB`` env var (a full path, or a directory containing it).
      2. The package directory (bundled wheel).
      3. Common in-tree build directories relative to the repo root.

    This performs filesystem probing only; it never dlopens the library.
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

    return None


# ---------------------------------------------------------------------------
# argtypes / restypes declaration
# ---------------------------------------------------------------------------


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

    # -- mesh I/O ------------------------------------------------------------
    # CyberStatus cyber_mesh_load_obj(const char* path, CyberMesh** out)
    lib.cyber_mesh_load_obj.argtypes = [c_char_p, POINTER(c_void_p)]
    lib.cyber_mesh_load_obj.restype = c_int32

    lib.cyber_mesh_save_obj.argtypes = [c_void_p, c_char_p]
    lib.cyber_mesh_save_obj.restype = c_int32

    # CyberStatus cyber_mesh_stats(const CyberMesh*, CyberStats* out)
    lib.cyber_mesh_stats.argtypes = [c_void_p, POINTER(CyberStatistics)]
    lib.cyber_mesh_stats.restype = c_int32

    lib.cyber_default_atlas_params.argtypes = [POINTER(CyberAtlasParams)]
    lib.cyber_default_atlas_params.restype = None
    lib.cyber_uv_atlas.argtypes = [c_void_p, POINTER(CyberAtlasParams), POINTER(CyberAtlasResult)]
    lib.cyber_uv_atlas.restype = c_int32

    # -- mesh queries --------------------------------------------------------
    lib.cyber_mesh_vertex_count.argtypes = [c_void_p]
    lib.cyber_mesh_vertex_count.restype = c_size_t

    lib.cyber_mesh_face_count.argtypes = [c_void_p]
    lib.cyber_mesh_face_count.restype = c_size_t

    # Copies up to `capacity` floats of packed xyz positions into `out`;
    # returns the total float count available (vertex_count * 3).
    lib.cyber_mesh_copy_positions.argtypes = [c_void_p, POINTER(c_float), c_size_t]
    lib.cyber_mesh_copy_positions.restype = c_size_t

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
