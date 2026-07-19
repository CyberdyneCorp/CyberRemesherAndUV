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
    print(f"  reference: {'ready' if qf else 'UNAVAILABLE (offline / no Eigen) — showing ours only'}\n")

    for name in args.models:
        path = c.download_model(name)
        src = c.load_any(path)

        # Uniform sizing (adaptivity=0) so the sizing matches QuadriFlow's. We
        # show BOTH quadrangulators: the default field-aligned matcher and the
        # Instant-Meshes position-field extractor (quad_method="instant-meshes"),
        # since the extractor is where the field-based edge flow lives — the
        # apples-to-apples fight with QuadriFlow's own field-based approach.
        def our_result(method: str) -> "tuple":
            mesh, _ = c.remesh_obj(
                path, c.RemeshParams(target_quad_count=args.target_quads, pure_quads=True,
                                     adaptivity=0.0, quad_method=method))
            q, _, _ = c.face_counts(mesh)
            return mesh, q, c.quad_quality(mesh)

        matched, mq, mqual = our_result("field-aligned")
        extracted, eq, equal = our_result("instant-meshes")

        def label(engine: str, n: int, q: "dict") -> str:
            return (f"{engine} · {n} quads\nmedian {q['median']:.0f}° · "
                    f"slivers {q['slivers']:.1f}% · CV {q['cv']:.2f}")

        panels = [src, matched, extracted]
        titles = [f"{name} · {len(src['faces'])} tris",
                  label("CyberRemesher (field-aligned)", mq, mqual),
                  label("CyberRemesher (position-field)", eq, equal)]

        if qf:
            ref = c.quadriflow_remesh(qf, path, eq)  # match the extractor's quad count
            rq, _, _ = c.face_counts(ref)
            rqual = c.quad_quality(ref)
            panels.append(ref)
            titles.append(label("QuadriFlow", rq, rqual))
            print(f"{name}: field-aligned median={mqual['median']:.0f}° cv={mqual['cv']:.2f} | "
                  f"position-field median={equal['median']:.0f}° slivers={equal['slivers']:.1f}% "
                  f"cv={equal['cv']:.2f} | QF median={rqual['median']:.0f}° "
                  f"slivers={rqual['slivers']:.1f}% cv={rqual['cv']:.2f}")
        else:
            print(f"{name}: field-aligned median={mqual['median']:.0f}° cv={mqual['cv']:.2f} | "
                  f"position-field median={equal['median']:.0f}° slivers={equal['slivers']:.1f}% "
                  f"cv={equal['cv']:.2f} (reference unavailable)")

        c.render_panels(panels, titles, os.path.join(c.OUTPUT_DIR, f"10_vs_{name}.png"),
                        suptitle=f"{name}: CyberRemesher (both quadrangulators) vs QuadriFlow")


if __name__ == "__main__":
    main()
