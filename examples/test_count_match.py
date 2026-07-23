#!/usr/bin/env python3
"""Regression tests for the benchmark's count-matching search.

Pure control-flow tests — no engine, no models, no network — so they run
anywhere. They exist because an unbounded version of this search hung the
benchmark: on a model whose achieved quad count saturates, the multiplicative
correction escalated the request to 40x desired (120k quads), which allocated
~4 GB and never returned.

    python examples/test_count_match.py     # or: pytest examples/test_count_match.py
"""

import importlib.util
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("bench", os.path.join(_HERE, "11_benchmark.py"))
bench = importlib.util.module_from_spec(_spec)
sys.modules["bench"] = bench
_spec.loader.exec_module(bench)

DESIRED = 3000


def _recording_probe(response):
    """Probe that records every requested target. `response(target) -> count`."""
    seen: list = []

    def probe(target: int):
        seen.append(target)
        return ("mesh", response(target))

    return probe, seen


def test_saturating_model_terminates_and_respects_ceiling():
    """The hang: achieved count pins at 916 regardless of the request (the
    stanford-bunny adaptive case). Must stop, and never request past the
    ceiling."""
    probe, seen = _recording_probe(lambda _t: 916)
    out = bench.search_matched_count(probe, DESIRED)
    assert out == "mesh", "must still return the best attempt"
    assert len(seen) <= 3, f"unbounded probing: {seen}"
    ceiling = DESIRED * bench._COUNT_MATCH_CEILING
    assert max(seen) <= ceiling, f"request {max(seen)} exceeded ceiling {ceiling}"


def test_slowly_growing_model_still_bounded():
    """Counts that creep up (past the 2% saturation guard) must still be capped
    by the ceiling rather than escalating indefinitely."""
    probe, seen = _recording_probe(lambda t: int(t * 0.2))
    bench.search_matched_count(probe, DESIRED)
    ceiling = DESIRED * bench._COUNT_MATCH_CEILING
    assert max(seen) <= ceiling, f"request {max(seen)} exceeded ceiling {ceiling}"
    assert len(seen) <= 3, f"unbounded probing: {seen}"


def test_converges_and_stops_early_when_close():
    """A well-behaved model lands inside tolerance on the first probe and must
    not spend further remeshes."""
    probe, seen = _recording_probe(lambda _t: DESIRED)
    assert bench.search_matched_count(probe, DESIRED) == "mesh"
    assert seen == [DESIRED], f"should stop on first hit, probed {seen}"


def test_returns_closest_attempt_not_last():
    """When nothing lands in tolerance, the CLOSEST attempt is returned — the
    last one may be worse."""
    counts = {DESIRED: 2900}  # near miss first, then a wild overshoot

    def response(target: int) -> int:
        return counts.get(target, 40000)

    def probe(target: int):
        return (f"mesh@{target}", response(target))

    assert bench.search_matched_count(probe, DESIRED, tol=0.001) == f"mesh@{DESIRED}"


def test_failed_probe_returns_none():
    """A remesh that fails outright yields None rather than raising."""
    assert bench.search_matched_count(lambda _t: (None, 0), DESIRED) is None


def test_ceiling_constant_stays_affordable():
    """Pin the ceiling itself. The other tests only check requests stay under
    whatever `_COUNT_MATCH_CEILING` happens to be, so they would keep passing if
    someone raised it — which is exactly the change that caused the hang (40x =
    a 120k-quad remesh, ~4 GB). Cost scales with the request, so this bound is a
    budget decision, not a correctness one: raise it only with a timing to back
    it up."""
    assert bench._COUNT_MATCH_CEILING <= 6.0, (
        f"ceiling {bench._COUNT_MATCH_CEILING}x risks pathological remesh cost")


def test_count_skew_is_symmetric_and_relative_to_the_smaller_arm():
    """`_count_skew` gates every count-sensitive comparison in Phases 1 and 3, so
    it must not depend on which arm is passed first, and it must measure the gap
    against the SMALLER count — otherwise a dense arm's advantage is understated
    exactly when it matters most."""
    a, b = {"quads": 2500}, {"quads": 3000}
    assert bench._count_skew(a, b) == bench._count_skew(b, a)
    assert bench._count_skew(a, b) == 0.2  # 500/2500, not 500/3000
    assert bench._count_skew(a, a) == 0.0
    assert bench._count_skew({"quads": 0}, b) == float("inf")


def test_skew_limit_rejects_the_density_gaps_that_faked_phase_3():
    """The artifact this guard exists to stop: at a fixed REQUEST of 3000,
    QuadriFlow came out 11-16% denser than the shipped default on four of five
    models (fandisk 2989 vs 2580, rocker-arm 2824 vs 2510, spot 2840 vs 2560,
    cheburashka 3052 vs 2686). `feature_error` falls roughly as count^-1/2, so
    those gaps are worth a large share of the very metric Phase 3 scored. Every
    one of them must land outside the limit, and a genuinely matched pair inside
    it."""
    historic = [(2580, 2989), (2510, 2824), (2560, 2840), (2686, 3052)]
    for ours, qf in historic:
        skew = bench._count_skew({"quads": ours}, {"quads": qf})
        assert skew > bench._COUNT_SKEW_LIMIT, (
            f"{ours}q vs {qf}q (skew {skew:.1%}) would still be scored")
    assert bench._count_skew({"quads": 3006}, {"quads": 2929}) <= bench._COUNT_SKEW_LIMIT


def test_quadriflow_tolerance_is_tighter_than_ours():
    """Both sides searching independently at the same REQUEST does not match them
    — each stops inside its own tolerance and they can settle on opposite sides
    (measured: spot 3228 vs 2840, 14% apart, both aiming at 3000). The reference
    arm therefore chases OUR achieved count with a tolerance tight enough that the
    result lands inside `_COUNT_SKEW_LIMIT`."""
    import inspect
    qf_tol = inspect.signature(bench.quadriflow_at_count).parameters["tol"].default
    ours_tol = inspect.signature(bench.ours_at_count).parameters["tol"].default
    assert qf_tol < ours_tol
    assert qf_tol <= bench._COUNT_SKEW_LIMIT


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS  {name}")
            except AssertionError as exc:  # noqa: PERF203 - report every failure
                failures += 1
                print(f"  FAIL  {name}: {exc}")
    print("all count-match regression tests passed" if not failures
          else f"{failures} test(s) failed")
    sys.exit(1 if failures else 0)
