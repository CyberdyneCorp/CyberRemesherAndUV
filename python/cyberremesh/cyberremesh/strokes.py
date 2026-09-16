"""Stroke interpretation: turning a drawn gesture into candidate actions.

Two stages, and the split matters. Stage one classifies the stroke's SHAPE from
its samples alone — a closed four-cornered ring, a circle, an X, a scribble.
Stage two resolves what the stroke passed OVER on the EditMesh and combines the
two into ranked candidate actions, so the same closed loop means "create a quad"
on empty surface and "hide this region" over existing faces.

Interpretation only: nothing here mutates a mesh. Applying a candidate is a
separate call through the build tools on :class:`~cyberremesh.Mesh`, which is
what lets a host offer the artist the one-tap alternatives before anything
changes.

This module exists because Python is the full-surface desktop test harness and
the gesture path was the one part it could not exercise — which is both the
hardest part to get right and the one most in need of corpus coverage.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from enum import IntEnum
from typing import List, Optional, Sequence, Tuple

from . import _ffi


class StrokeShape(IntEnum):
    """What the stroke looks like, from its samples alone."""

    UNKNOWN = 0
    HOLD_POINT = 1  #: a stationary press (tap/hold)
    LINE = 2  #: open and straight
    CLOSED_LOOP = 3  #: a closed polygon with corners (the quad-draw gesture)
    CIRCLE = 4  #: closed and round
    SCRIBBLE = 5  #: open, many reversals or self-crossings
    CROSS = 6  #: an X drawn in one stroke
    LASSO = 7  #: closed and irregular
    GRID = 8  #: an open square wave (the one-stroke grid)


class StrokeContext(IntEnum):
    """What the resolver found under the stroke."""

    EMPTY_SURFACE = 0
    FACE = 1
    EDGE = 2
    BOUNDARY_EDGE = 3
    VERTEX = 4


class StrokeAction(IntEnum):
    """A candidate action from the gesture grammar."""

    NONE = 0
    CREATE_QUAD = 1
    INSERT_LOOP = 2
    TAG_LOOP = 3
    DISSOLVE_EDGE = 4
    DELETE_FACES = 5
    MERGE_VERTICES = 6
    ROTATE_EDGE = 7
    TWEAK_VERTEX = 8
    HIDE_REGION = 9
    TOGGLE_VISIBILITY = 10
    CREATE_GRID = 11


class ElementKind(IntEnum):
    VERTEX = 0
    EDGE = 1
    FACE = 2


@dataclass(frozen=True)
class Candidate:
    """One ranked interpretation of a stroke.

    ``elements`` are the mesh elements this candidate would touch, in
    deterministic order, as ``(kind, id)`` pairs.
    """

    action: StrokeAction
    confidence: float
    elements: Tuple[Tuple[ElementKind, int], ...]


@dataclass(frozen=True)
class Interpretation:
    """The result of interpreting one completed stroke.

    ``candidates`` is ranked best-first: index 0 is the chosen interpretation
    and the rest are the one-tap alternatives a host can offer.

    ``corners`` holds the estimated corner points of a CLOSED stroke in
    normalized viewport coordinates — the quad a host should unproject onto the
    Target when applying a ``CREATE_QUAD`` candidate. It is empty for open
    shapes. For a ``GRID`` stroke it instead holds the whole lattice, row-major,
    and ``grid_size`` gives its ``(rows, cols)`` of quad cells.
    """

    shape: StrokeShape
    shape_confidence: float
    context: StrokeContext
    candidates: Tuple[Candidate, ...]
    corners: Tuple[Tuple[float, float], ...]
    grid_size: Optional[Tuple[int, int]]

    @property
    def action(self) -> StrokeAction:
        """The chosen action, or ``NONE`` when nothing was recognised."""
        return self.candidates[0].action if self.candidates else StrokeAction.NONE


def interpret(
    samples: Sequence[Sequence[float]],
    edit_mesh=None,
    view_proj: Optional[Sequence[float]] = None,
    aspect: float = 1.0,
) -> Interpretation:
    """Interpret one completed stroke.

    ``samples`` are ``(x, y, t)`` triplets: normalized viewport coordinates in
    ``[0, 1]`` with the origin top-left, and seconds since the stroke began.
    Timing is part of the input because a stationary press and a fast tap are
    different gestures with identical geometry.

    ``edit_mesh`` and ``view_proj`` may both be ``None``: stage one still runs
    and every context rule then sees an empty scene, which is the right answer
    for a stroke drawn before any topology exists. Supplying one without the
    other is refused by the engine.

    ``view_proj`` is a column-major 4x4 world-to-clip matrix as 16 floats.
    ``aspect`` is viewport width/height, so circles and angles are measured
    undistorted.

    Deterministic: identical inputs produce identical results.
    """
    lib = _ffi.get_lib()
    flat: List[float] = []
    for sample in samples:
        flat.extend((float(sample[0]), float(sample[1]), float(sample[2])))
    if not flat:
        raise ValueError("interpret needs at least one (x, y, t) sample")
    buf = (ctypes.c_float * len(flat))(*flat)

    proj = None
    if view_proj is not None:
        if len(view_proj) != 16:
            raise ValueError("view_proj must be 16 floats (column-major 4x4)")
        proj = (ctypes.c_float * 16)(*[float(v) for v in view_proj])

    handle = ctypes.c_void_p()
    status = lib.cyber_stroke_interpret(
        edit_mesh.handle if edit_mesh is not None else None,
        proj, buf, len(flat) // 3, float(aspect), ctypes.byref(handle),
    )
    if status != _ffi.STATUS_OK:
        from .api import CyberError, _last_error

        raise CyberError(status, _last_error())

    try:
        return _read(lib, handle)
    finally:
        lib.cyber_stroke_interpretation_free(handle)


def _read(lib, handle) -> Interpretation:
    """Copies everything out before the caller frees the record."""
    candidates: List[Candidate] = []
    for index in range(lib.cyber_stroke_interpretation_candidate_count(handle)):
        elements: List[Tuple[ElementKind, int]] = []
        for slot in range(lib.cyber_stroke_interpretation_element_count(handle, index)):
            kind = ctypes.c_int32(0)
            element_id = ctypes.c_uint32(0)
            if lib.cyber_stroke_interpretation_element(
                handle, index, slot, ctypes.byref(kind), ctypes.byref(element_id)
            ):
                elements.append((ElementKind(kind.value), element_id.value))
        candidates.append(
            Candidate(
                action=StrokeAction(lib.cyber_stroke_interpretation_action(handle, index)),
                confidence=float(
                    lib.cyber_stroke_interpretation_confidence(handle, index)
                ),
                elements=tuple(elements),
            )
        )

    corners: List[Tuple[float, float]] = []
    for slot in range(lib.cyber_stroke_interpretation_corner_count(handle)):
        xy = (ctypes.c_float * 2)()
        if lib.cyber_stroke_interpretation_corner(handle, slot, xy):
            corners.append((float(xy[0]), float(xy[1])))

    rows = ctypes.c_size_t(0)
    cols = ctypes.c_size_t(0)
    has_grid = lib.cyber_stroke_interpretation_grid_size(
        handle, ctypes.byref(rows), ctypes.byref(cols)
    )

    return Interpretation(
        shape=StrokeShape(lib.cyber_stroke_interpretation_shape(handle)),
        shape_confidence=float(lib.cyber_stroke_interpretation_shape_confidence(handle)),
        context=StrokeContext(lib.cyber_stroke_interpretation_context(handle)),
        candidates=tuple(candidates),
        corners=tuple(corners),
        grid_size=(rows.value, cols.value) if has_grid else None,
    )
