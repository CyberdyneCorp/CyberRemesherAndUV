#!/usr/bin/env python3
"""Phase-1 retopology benchmark: score CyberRemesher vs QuadriFlow on the quality
that actually matters — how faithfully the quads follow the source surface, how
clean the singularity structure is, and the usual angle/uniformity — across the
community test-model corpus at matched polygon count.

It also runs the Phase-2 adaptivity comparison: ours with curvature-adaptive sizing
vs ours with uniform sizing, at matched *achieved* quad count (QuadriFlow shown for
context). Adaptive sizing spends polygons where the surface bends, so it should
reproduce the surface more accurately per polygon — something QuadriFlow's uniform
grid cannot do. Phase 2 necessarily runs on the position-field extractor, the only
one exposing the knob; the shipped quad-cover default is uniform-only by design.

    examples/run.sh examples/11_benchmark.py
    examples/run.sh examples/11_benchmark.py --models spot fandisk --target-quads 3000

Writes a scored summary chart to output/11_benchmark.png. If QuadriFlow cannot be
built (offline / no Eigen), the reference columns are omitted and ours still run.
"""

import argparse
import os
import sys

# `common` drags in matplotlib + numpy (its plotting/mesh helpers). The
# count-match regression test loads this module only for its pure-control-flow
# helpers (search_matched_count, _count_skew, the constants), so guard the import
# to keep the module loadable on a bare interpreter — CI runners have no plotting
# stack. An actual benchmark run needs `common` and will fail loudly if it is None.
try:
    import common as c
except ImportError:
    c = None

DEFAULT_MODELS = ["spot", "fandisk", "rocker-arm", "cheburashka", "stanford-bunny"]

# Metrics and whether lower is better. These are the benchmark's North Star.
METRICS = [
    ("median", "median angle°", False),   # higher better
    ("rms", "surface dev %", True),        # lower better
    ("normal_err", "normal err°", True),   # lower better
    ("irregular", "irregular %", True),    # lower better
    ("cv", "edge CV", True),               # lower better
]


def evaluate(mesh: "c.MeshData", source: "c.MeshData") -> dict:
    """All benchmark metrics for one output mesh against its source."""
    q, _, _ = c.face_counts(mesh)
    qq = c.quad_quality(mesh)
    sm = c.surface_metrics(mesh, source)
    fe = c.feature_error(mesh, source)
    val = c.mesh_validity(mesh)
    return {
        "quads": q,
        "median": qq["median"],
        "cv": qq["cv"],
        "slivers": qq["slivers"],
        "rms": sm["rms"],
        "normal_err": sm["normal_err"],
        "irregular": c.irregular_pct(mesh),
        "feature": fe if fe is not None else float("nan"),
        "defects": val["nonmanifold"] + val["boundary"] + val["degenerate"],
    }


def ours(path: str, target: int, method: str, adaptivity: float) -> "c.MeshData | None":
    try:
        mesh, _ = c.remesh_obj(
            path, c.RemeshParams(target_quad_count=target, pure_quads=True,
                                 adaptivity=adaptivity, quad_method=method))
        return mesh
    except Exception:  # noqa: BLE001
        return None


# Hard ceiling on the count-match request, as a multiple of the desired count.
# The request drives mesh resolution, so cost grows with it: an unbounded
# multiplicative correction escalated a non-converging model to 40x (120k quads),
# which allocated ~4 GB and never returned. If 4x the desired count still cannot
# reach the density, the extractor simply saturates there — keep the best attempt
# and report the miss rather than chasing it.
_COUNT_MATCH_CEILING = 4.0


def search_matched_count(probe, desired: int, tol: float = 0.08, iters: int = 3):
    """Bounded search for a request whose ACHIEVED count lands near `desired`.

    `probe(target) -> (payload, achieved_count)`; returns the payload of the
    closest attempt, or None if the first probe failed. Pure control flow with no
    engine dependency, so the termination guarantees below are unit-testable
    (see test_count_match.py).

    Bounded on every axis — each guard exists because its absence hangs:
      * at most `iters` probes;
      * the request never exceeds `_COUNT_MATCH_CEILING` x `desired`;
      * stop once a raise buys < 2% more (the extractor has saturated);
      * stop once the correction can no longer raise the request.
    """
    ceiling = int(desired * _COUNT_MATCH_CEILING)
    target, best, best_err, prev_got = desired, None, float("inf"), 0
    for _ in range(iters):
        payload, got = probe(target)
        if payload is None or got <= 0:
            return best
        err = abs(got - desired) / desired
        if err < best_err:
            best, best_err = payload, err
        if err <= tol:
            return payload
        # Saturated: the last raise bought < 2% more quads, so further raises are
        # just cost. Stop instead of escalating into a pathological resolution.
        if prev_got and got <= prev_got * 1.02:
            return best
        prev_got = got
        # Achieved count rises with the request; correct multiplicatively, bounded.
        nxt = int(max(desired * 0.25, min(ceiling, target * desired / got)))
        if nxt <= target:  # already at the ceiling and still short — no point retrying
            return best
        target = nxt
    return best


def ours_at_count(path: str, desired: int, method: str, adaptivity: float,
                  tol: float = 0.08, iters: int = 3) -> "c.MeshData | None":
    """Remesh so the ACHIEVED quad count lands near `desired`.

    `target_quad_count` is a request, not a guarantee — the extractors undershoot
    it, and curvature-adaptive sizing undershoots it severely (spot: target 3000 ->
    268 achieved). Comparing an adaptive run against a uniform run at the same
    *requested* target therefore compares two different, and often degenerate,
    densities. Rescale the request until the output count is within `tol`, so
    per-polygon fidelity is a fair comparison.

    Returns the closest attempt, which the caller must check against its
    counterpart — a miss is reported, never silently scored."""
    def probe(target: int):
        mesh = ours(path, target, method, adaptivity)
        return (None, 0) if mesh is None else (mesh, c.face_counts(mesh)[0])

    return search_matched_count(probe, desired, tol, iters)


def quadriflow_at_count(qf: "str | None", path: str, desired: int,
                        tol: float = 0.04, iters: int = 4) -> "c.MeshData | None":
    """QuadriFlow driven so its ACHIEVED quad count lands near `desired`.

    `-f` is a request there too, and QuadriFlow historically overshot ours by
    11-16% at the same request (fandisk 2989 vs 2580, rocker-arm 2824 vs 2510).
    That matters more than it looks: feature-following error falls roughly as
    count^-1/2 — measured on fandisk, about -0.02 percentage points per +100 quads
    — so a 400-quad density advantage is worth ~0.08 of the very metric Phase 3
    scores, for free.

    `desired` must be OUR ACHIEVED count, not the benchmark request. Driving both
    sides independently at the request does not match them: each stops as soon as
    it is within its own tolerance, so two arms can settle on opposite sides of it
    and end up further apart than if neither had searched (measured: spot 3228 vs
    2840, 14% apart, when both aimed at 3000). The tolerance here is therefore
    tighter than `ours_at_count`'s, since this arm is chasing a fixed number rather
    than defining one."""
    def probe(target: int):
        mesh = c.quadriflow_try(qf, path, target)
        return (None, 0) if mesh is None else (mesh, c.face_counts(mesh)[0])

    return search_matched_count(probe, desired, tol, iters)


# Largest relative count difference at which two arms may still be compared on a
# count-sensitive metric. Matches `search_matched_count`'s own tolerance: if the
# bounded search could not close the gap, the pair is reported as a miss rather
# than scored. Phase 2 has carried this guard for a while; Phase 3 did not, which
# is how a density advantage could read as a quality result.
_COUNT_SKEW_LIMIT = 0.08


def _count_skew(a: dict, b: dict) -> float:
    """Relative quad-count difference between two evaluated arms."""
    lo = min(a["quads"], b["quads"])
    return abs(a["quads"] - b["quads"]) / lo if lo > 0 else float("inf")


def main() -> None:
    parser = argparse.ArgumentParser(description="CyberRemesher vs QuadriFlow benchmark")
    parser.add_argument("--models", nargs="+", default=DEFAULT_MODELS,
                        help=f"choices: {sorted(c.COMMON_3D_MODELS)}")
    parser.add_argument("--target-quads", type=int, default=3000)
    args = parser.parse_args()

    c.require_engine()
    print("building QuadriFlow reference (first run compiles it)...")
    qf = c.quadriflow_binary()
    print(f"  QuadriFlow: {'ready' if qf else 'UNAVAILABLE'}")
    print("building AutoRemesher reference (first run compiles QuadCover + Geogram)...")
    ar = c.autoremesher_binary()
    print(f"  AutoRemesher: {'ready' if ar else 'UNAVAILABLE'}")
    # "ours quad-cover" is the default method. Built with -DCYBER_WITH_QUADCOVER=ON
    # (the cpu-headless preset) it uses the in-process Geogram field that beats
    # QuadriFlow on organic meshes; otherwise the dependency-free native seamless-UV
    # solver (a few degrees lower median).
    print("  ours quad-cover: default method (Geogram field under -DCYBER_WITH_QUADCOVER)\n")

    header = f"{'model':<15} {'engine':<20} {'quads':>6} {'med°':>5} {'dev%':>6} {'Nerr°':>6} {'irr%':>6} {'CV':>5}"
    print(header)
    print("-" * len(header))

    rows: list = []          # (model, engine, metrics) for uniform comparison
    adaptive_rows: list = []  # (model, our_adaptive_metrics, qf_metrics) for Phase 2
    wins = {m[0]: [0, 0] for m in METRICS}  # metric -> [ours_better, total] vs QF

    # Unbuffered: this run is long, and a redirected stdout otherwise hides all
    # progress, so a stall is indistinguishable from slow work.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:  # pragma: no cover - non-reconfigurable stream
        pass

    for name in args.models:
        print(f"[{name}] ...", flush=True)
        try:
            path = c.download_model(name)
            src = c.load_any(path)
        except Exception as exc:  # noqa: BLE001
            print(f"{name:<15} SKIP ({exc})")
            continue

        # --- Phase 1: uniform, matched ACHIEVED count — apples-to-apples on every metric.
        #
        # Every arm is driven through the same bounded count search, rather than
        # merely handed the same REQUEST. The request is not a guarantee and the
        # engines miss it by different amounts, so the old "matched count" label was
        # aspirational: QuadriFlow came out 11-16% denser than our default on four of
        # five models, which flatters it on every density-sensitive metric (feature
        # error above all — see `quadriflow_at_count`).
        #
        # The shipped default (quad-cover) is the ANCHOR: our arms aim at
        # --target-quads, then QuadriFlow is driven to whatever quad-cover ACHIEVED.
        # Pointing both sides at the request instead does not match them — each stops
        # inside its own tolerance and they can settle on opposite sides of it.
        # AutoRemesher stays on a plain request: it is nondeterministic run-to-run,
        # so iterating on its achieved count would chase noise, and it is printed for
        # context rather than scored.
        engines = {}
        fa = ours_at_count(path, args.target_quads, "field-aligned", 0.0)
        im = ours_at_count(path, args.target_quads, "instant-meshes", 0.0)
        intg = ours_at_count(path, args.target_quads, "integer", 0.0)
        qc = ours_at_count(path, args.target_quads, "quad-cover", 0.0)
        if fa:
            engines["ours field-aligned"] = evaluate(fa, src)
        if im:
            engines["ours position-field"] = evaluate(im, src)
        if intg:
            engines["ours integer"] = evaluate(intg, src)
        if qc:
            engines["ours quad-cover"] = evaluate(qc, src)
        anchor = c.face_counts(qc)[0] if qc else args.target_quads
        ref = quadriflow_at_count(qf, path, anchor)
        if ref:
            engines["QuadriFlow"] = evaluate(ref, src)
        aref = c.autoremesher_try(ar, path, args.target_quads)
        if aref:
            engines["AutoRemesher"] = evaluate(aref, src)

        for engine, mtr in engines.items():
            print(f"{name:<15} {engine:<20} {mtr['quads']:>6} {mtr['median']:>5.0f} "
                  f"{mtr['rms']:>6.2f} {mtr['normal_err']:>6.1f} {mtr['irregular']:>6.0f} {mtr['cv']:>5.2f}")
            rows.append((name, engine, mtr))

        # Best-of-ours vs QuadriFlow, per metric — over the arms that are actually
        # COMPARABLE at this density. Surface deviation, normal error and
        # irregular-vertex share all improve with density, so "best of ours" taken
        # across arms of wildly different counts is not a like-for-like maximum: an
        # arm that overshot to 4278 quads against QuadriFlow's 2929 would win on
        # deviation for the density alone. QuadriFlow is matched to the shipped
        # default's achieved count, so arms within `_COUNT_SKEW_LIMIT` of it are
        # scorable and the rest are reported and dropped. If none qualifies the
        # model contributes nothing, rather than contributing a density artifact.
        if ref:
            comparable = {e: m for e, m in engines.items()
                          if e.startswith("ours")
                          and _count_skew(m, engines["QuadriFlow"]) <= _COUNT_SKEW_LIMIT}
            dropped = [e for e in engines
                       if e.startswith("ours") and e not in comparable]
            if dropped:
                print(f"{name:<15} not count-comparable to QuadriFlow "
                      f"({engines['QuadriFlow']['quads']}q): "
                      + ", ".join(f"{e.removeprefix('ours ')} {engines[e]['quads']}q"
                                  for e in dropped))
            for key, _, lower in METRICS:
                vals = [m[key] for m in comparable.values()]
                if not vals:
                    continue
                best = min(vals) if lower else max(vals)
                qfv = engines["QuadriFlow"][key]
                wins[key][0] += int(best < qfv if lower else best > qfv)
                wins[key][1] += 1

        # --- Phase 2: does curvature-adaptive sizing improve fidelity per polygon?
        # Compare ours-adaptive against ours-UNIFORM at the same ACHIEVED count
        # (isolates the sizing benefit from the mesh-quality gap that also separates
        # us from QuadriFlow), with QuadriFlow at that count for context.
        #
        # This runs on "instant-meshes", NOT on the shipped quad-cover default:
        # quad-cover is deliberately uniform-only (capi.cpp hardcodes adaptivity 0
        # and the pipeline's isotropic stage — the only consumer of params.adaptivity
        # — is bypassed for it), so an adaptive/uniform split on the default would
        # compare a mesh against itself. This measures the adaptivity LEVER, and says
        # nothing about the default's fidelity; Phase 1 covers that.
        #
        # The adaptive arm is driven toward target_quads, then the uniform arm is
        # matched to whatever the adaptive arm ACHIEVED, so the pair is comparable.
        # The old code instead requested `achieved * 1.3` once, which left the two
        # arms 20-30% apart down at ~200-400 quads, where both extractors degrade —
        # that regime produced fandisk's 22.61% "uniform" deviation and the bogus
        # WIN scored against it.
        #
        # CAVEAT: adaptive sizing undershoots the request ~4-5x, and the ceiling in
        # ours_at_count bounds how far the request may be raised to compensate. So on
        # models where adaptive saturates early the pair is matched to each other but
        # sits below target_quads (spot lands ~657q). Cross-MODEL comparison of these
        # rows is therefore not meaningful; each row is only self-consistent.
        ad = ours_at_count(path, args.target_quads, "instant-meshes", 1.0)
        if ad:
            am = evaluate(ad, src)
            uni = ours_at_count(path, am["quads"], "instant-meshes", 0.0)
            qf_matched = c.quadriflow_try(qf, path, am["quads"])
            if uni:
                adaptive_rows.append((name, am, evaluate(uni, src),
                                      evaluate(qf_matched, src) if qf_matched else None))
        print()

    _print_verdict(wins, adaptive_rows)
    _phase3(rows)
    _render(rows, adaptive_rows, args.target_quads)


def _phase3(rows: list) -> None:
    """Phase 3 — features & robustness: the SHIPPED default (quad-cover) vs
    QuadriFlow on feature-following error (edges on sharp creases) and topological
    defects (non-manifold edges + holes/tears + degenerate faces).

    Scored on the default. The retired position-field extractor is printed beside it
    for continuity with the historical numbers in docs/ROADMAP.md — its defect counts
    (cheburashka 110, bunny 92) are what the roadmap's open "real extractor bug"
    follow-up refers to, and they say nothing about what ships today.

    COUNT SENSITIVITY. feature-follow error is a distance from source crease samples
    to the nearest output edge, so it improves mechanically with output edge density
    — measured on fandisk at roughly -0.02 points per +100 quads, against a
    within-arm residual spread of ~0.04. A pair whose achieved counts differ by more
    than `_COUNT_SKEW_LIMIT` therefore cannot be scored on it, and is printed as SKEW
    instead of counted as a win or a loss. Both arms come out of Phase 1's bounded
    achieved-count search, so this should normally be a no-op; when it fires the
    honest reading is "no comparison", not "we lost"."""
    import math as _m

    def _fmt(mtr: dict) -> str:
        return "n/a" if _m.isnan(mtr["feature"]) else f"{mtr['feature']:.2f}%"

    models = sorted({r[0] for r in rows})
    print("\nPHASE 3 — features & robustness (quad-cover DEFAULT vs QuadriFlow):")
    print("  feature-follow % (edges on source creases, lower better) · topo defects (count, lower better)")
    print(f"  feature scored only where the two arms' quad counts agree to {_COUNT_SKEW_LIMIT:.0%}")
    def pick(model: str, engine: str) -> "dict | None":
        return next((r[2] for r in rows if r[0] == model and r[1] == engine), None)

    feat_wins = feat_total = def_wins = total = 0
    for name in models:
        o, q = pick(name, "ours quad-cover"), pick(name, "QuadriFlow")
        legacy = pick(name, "ours position-field")
        if not o or not q:
            continue
        total += 1
        # Topological defects count broken elements, not a density-scaled average,
        # so they stay scored whatever the skew.
        def_wins += o["defects"] <= q["defects"]
        skew = _count_skew(o, q)
        scorable = (skew <= _COUNT_SKEW_LIMIT and not _m.isnan(o["feature"])
                    and not _m.isnan(q["feature"]))
        if scorable:
            feat_total += 1
            feat_wins += o["feature"] <= q["feature"]
        note = f"  [SKEW {skew:.0%}: feature not scored]" if not scorable else ""
        legacy_txt = f"  (retired position-field: {_fmt(legacy)}/{legacy['defects']})" if legacy else ""
        print(f"  {name:<15} feature ours={_fmt(o):<7} QF={_fmt(q):<7} "
              f"({o['quads']}q vs {q['quads']}q) | "
              f"defects ours={o['defects']:<4} QF={q['defects']}{note}{legacy_txt}")
    if total:
        feat_txt = f"{feat_wins}/{feat_total}" if feat_total else "not scored (every pair skewed)"
        print(f"  default ≥ QuadriFlow: feature-follow {feat_txt} · "
              f"topo defects {def_wins}/{total}")


def _print_verdict(wins: dict, adaptive_rows: list) -> None:
    print("=" * 60)
    print("PHASE 1 — ours (best quadrangulator) vs QuadriFlow, per metric:")
    for key, label, _ in METRICS:
        won, total = wins[key]
        if total:
            print(f"  {label:<16} ours better on {won}/{total} models")
    if adaptive_rows:
        print("\nPHASE 2 — adaptivity: does curvature-adaptive sizing beat uniform per polygon?")
        print("  (ours adaptive vs ours uniform at matched count — lower surface dev is better;")
        print("   measured on the position-field extractor: the quad-cover DEFAULT is")
        print("   uniform-only, so it has no adaptive/uniform split to compare.")
        print("   QuadriFlow shown for context — beating it absolutely is Phase-4-gated)")
        better = scored = 0
        for name, ad, uni, ref in adaptive_rows:
            # A run whose count missed the other side's, or whose deviation blew up,
            # is a degenerate extraction — report it, never score it as a win.
            skew = abs(ad["quads"] - uni["quads"]) / max(1, uni["quads"])
            bad = max(ad["rms"], uni["rms"])
            if skew > 0.15:
                mark = "SKIP"  # counts never converged; not an apples-to-apples pair
            elif bad > 5.0:
                mark = "DEGN"  # implausible surface deviation -> extraction failed
            else:
                scored += 1
                better += ad["rms"] < uni["rms"]
                mark = "WIN " if ad["rms"] < uni["rms"] else "----"
            qtxt = f"  ·  QF {ref['rms']:.2f}%" if ref else ""
            print(f"  {mark} {name:<15} adaptive {ad['rms']:.2f}% ({ad['quads']}q)  vs  "
                  f"uniform {uni['rms']:.2f}% ({uni['quads']}q){qtxt}")
        print(f"  adaptive beats uniform per-polygon: {better}/{scored} models scored"
              f" ({len(adaptive_rows) - scored} excluded as degenerate/unmatched)")
    print("=" * 60)


def _render(rows: list, adaptive_rows: list, target: int) -> None:
    if not rows:
        print("no results to render")
        return
    import matplotlib.pyplot as plt
    import numpy as np
    models = sorted({r[0] for r in rows}, key=lambda m: m)
    engines = ["ours position-field", "ours integer", "ours quad-cover", "QuadriFlow", "AutoRemesher"]
    colors = {"ours field-aligned": "#5b9bd5", "ours position-field": "#2e8b57",
              "ours integer": "#7b4fa0", "ours quad-cover": "#e64980",
              "QuadriFlow": "#c05640", "AutoRemesher": "#d98e2b"}
    panel_metrics = [("median", "median angle° (higher better)", False),
                     ("rms", "surface dev % (lower better)", True),
                     ("irregular", "irregular vertices % (lower better)", True)]

    fig, axes = plt.subplots(1, len(panel_metrics) + (1 if adaptive_rows else 0),
                             figsize=(5.2 * (len(panel_metrics) + (1 if adaptive_rows else 0)), 4.6),
                             dpi=130)
    fig.suptitle(f"CyberRemesher vs QuadriFlow — retopology benchmark "
                 f"(~{target} quads, uniform)", fontsize=14, fontweight="bold", color="#12233a")
    x = np.arange(len(models))
    width = 0.16
    for ax, (key, label, _lower) in zip(axes, panel_metrics):
        for e_i, engine in enumerate(engines):
            vals = []
            for m in models:
                hit = [r[2][key] for r in rows if r[0] == m and r[1] == engine]
                vals.append(hit[0] if hit else 0.0)
            ax.bar(x + (e_i - (len(engines) - 1) / 2) * width, vals, width, label=engine,
                   color=colors[engine])
        ax.set_xticks(x)
        ax.set_xticklabels(models, rotation=30, ha="right", fontsize=9)
        ax.set_title(label, fontsize=11)
        ax.grid(axis="y", alpha=0.25)
    axes[0].legend(fontsize=8, loc="lower right")

    if adaptive_rows:
        ax = axes[len(panel_metrics)]
        am = [r[0] for r in adaptive_rows]
        xa = np.arange(len(am))
        ax.bar(xa - width / 2, [r[1]["rms"] for r in adaptive_rows], width,
               label="ours adaptive", color="#2e8b57")
        ax.bar(xa + width / 2, [r[2]["rms"] for r in adaptive_rows], width,
               label="ours uniform", color="#5b9bd5")
        ax.set_xticks(xa)
        ax.set_xticklabels(am, rotation=30, ha="right", fontsize=9)
        ax.set_title("surface dev % — adaptive vs uniform\n(ours, matched count; lower better)", fontsize=11)
        ax.grid(axis="y", alpha=0.25)
        ax.legend(fontsize=8)

    fig.tight_layout(rect=(0, 0, 1, 0.94))
    out = os.path.join(c.OUTPUT_DIR, "11_benchmark.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {os.path.relpath(out, c._REPO)}")


if __name__ == "__main__":
    main()
