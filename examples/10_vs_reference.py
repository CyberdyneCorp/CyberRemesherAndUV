#!/usr/bin/env python3
"""Side-by-side quad retopology: CyberRemesher vs a field-based reference.

AutoRemesher is a GUI-only app and cannot be scripted headless, so this compares
against QuadriFlow (github.com/hjwdzh/QuadriFlow) — a field-based / integer-grid
quad remesher in the same state-of-the-art lineage (Instant Meshes family).
QuadriFlow is built on demand (headless, boost-free) by reference/build_quadriflow.sh.

Both engines produce 100% quads, so this compares EDGE FLOW and QUAD SHAPE at a
matched density and matched (uniform) sizing — QuadriFlow sizes uniformly, so we
run with adaptivity=0 for an apples-to-apples fight (CyberRemesher can also size
adaptively by curvature, which QuadriFlow's default does not). Reported per mesh:
median smallest-quad-angle (higher = better), sliver rate (% of quads under 20
deg, lower = better), and edge-length CV (lower = more uniform).

    examples/run.sh examples/10_vs_reference.py
    examples/run.sh examples/10_vs_reference.py --models spot bunny

If QuadriFlow cannot be built (offline / no Eigen), the script still renders our
own result and says the reference was unavailable.
"""

import argparse
import os

import common as c

DEFAULT_MODELS = ["spot", "fandisk", "stanford-bunny"]


def main() -> None:
    parser = argparse.ArgumentParser(description="CyberRemesher vs QuadriFlow")
    parser.add_argument("--models", nargs="+", default=DEFAULT_MODELS,
                        help=f"choices: {sorted(c.COMMON_3D_MODELS)}")
    parser.add_argument("--target-quads", type=int, default=2500)
    args = parser.parse_args()

    c.require_engine()
    print("building QuadriFlow reference (first run compiles it)...")
    qf = c.quadriflow_binary()
    print(f"  QuadriFlow: {'ready' if qf else 'UNAVAILABLE (offline / no Eigen)'}")
    print("building AutoRemesher reference (first run compiles QuadCover + Geogram)...")
    ar = c.autoremesher_binary()
    print(f"  AutoRemesher: {'ready' if ar else 'UNAVAILABLE'}")
    # ours quad-cover uses the NATIVE (dependency-free) seamless-UV solver by default; do not
    # set CYBER_QUADCOVER_CLI (set it in the shell to compare the vendored Geogram path).
    print("  ours quad-cover: default method (Geogram field when built -DCYBER_WITH_QUADCOVER)\n")

    # Every CyberRemesher quadrangulator, so the visual comparison shows them all.
    OUR_METHODS = [
        ("field-aligned", "field-aligned"),
        ("position-field", "instant-meshes"),
        ("integer", "integer"),
        ("quad-cover", "quad-cover"),
    ]

    def label(engine: str, n: int, q: "dict") -> str:
        return (f"{engine} · {n} quads\nmedian {q['median']:.0f}° · "
                f"slivers {q['slivers']:.1f}% · CV {q['cv']:.2f}")

    for name in args.models:
        path = c.download_model(name)
        src = c.load_any(path)

        # Uniform sizing (adaptivity=0) so the sizing matches QuadriFlow's, matched count.
        def our_result(method: str) -> "tuple":
            mesh, _ = c.remesh_obj(
                path, c.RemeshParams(target_quad_count=args.target_quads, pure_quads=True,
                                     adaptivity=0.0, quad_method=method))
            q, _, _ = c.face_counts(mesh)
            return mesh, q, c.quad_quality(mesh)

        panels = [src]
        titles = [f"{name} · {len(src['faces'])} tris"]
        matched_count = args.target_quads
        summary = []
        for display, method in OUR_METHODS:
            try:
                mesh, q, qual = our_result(method)
            except Exception as exc:  # noqa: BLE001 - a method may be unavailable
                print(f"{name}: {display} SKIP ({exc})")
                continue
            if method == "quad-cover":
                matched_count = q  # match references to the headline method
            panels.append(mesh)
            titles.append(label(f"CyberRemesher ({display})", q, qual))
            summary.append(f"{display} med={qual['median']:.0f}° irr={c.irregular_pct(mesh):.0f}% "
                           f"cv={qual['cv']:.2f}")

        if qf:
            ref = c.quadriflow_remesh(qf, path, matched_count)
            rq, _, _ = c.face_counts(ref)
            rqual = c.quad_quality(ref)
            panels.append(ref)
            titles.append(label("QuadriFlow", rq, rqual))
            summary.append(f"QF med={rqual['median']:.0f}° irr={c.irregular_pct(ref):.0f}% "
                           f"cv={rqual['cv']:.2f}")

        aref = c.autoremesher_try(ar, path, matched_count)
        if aref is not None:
            arq, _, _ = c.face_counts(aref)
            panels.append(aref)
            titles.append(label("AutoRemesher", arq, c.quad_quality(aref)))

        print(f"{name}: " + " | ".join(summary))
        c.render_grid(panels, titles, os.path.join(c.OUTPUT_DIR, f"10_vs_{name}.png"), cols=4,
                      suptitle=f"{name}: CyberRemesher (all methods) vs QuadriFlow vs AutoRemesher")


if __name__ == "__main__":
    main()
