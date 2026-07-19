#!/usr/bin/env python3
"""Side-by-side quad retopology: CyberRemesher vs a field-based reference.

AutoRemesher is a GUI-only app and cannot be scripted headless, so this compares
against QuadriFlow (github.com/hjwdzh/QuadriFlow) — a field-based / integer-grid
quad remesher in the same state-of-the-art lineage (Instant Meshes family).
QuadriFlow is built on demand (headless, boost-free) by reference/build_quadriflow.sh.

Both engines produce 100% quads, so this compares EDGE FLOW and QUAD SHAPE at a
matched density, with numbers: worst quad interior angle (closer to 90 is better)
and edge-length coefficient of variation (lower = more uniform).

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

        ours, _ = c.remesh_obj(
            path, c.RemeshParams(target_quad_count=args.target_quads, pure_quads=True))
        oq, _, _ = c.face_counts(ours)
        o_ang, o_cv = c.quad_quality(ours)

        panels = [src, ours]
        titles = [f"{name} · {len(src['faces'])} tris",
                  f"CyberRemesher · {oq} quads\nworst angle {o_ang:.0f}° · edge CV {o_cv:.2f}"]

        if qf:
            ref = c.quadriflow_remesh(qf, path, oq)  # match our quad count
            rq, _, _ = c.face_counts(ref)
            r_ang, r_cv = c.quad_quality(ref)
            panels.append(ref)
            titles.append(f"QuadriFlow · {rq} quads\nworst angle {r_ang:.0f}° · edge CV {r_cv:.2f}")
            print(f"{name}: ours worst-angle={o_ang:.0f}° cv={o_cv:.2f} | "
                  f"QuadriFlow worst-angle={r_ang:.0f}° cv={r_cv:.2f}")
        else:
            print(f"{name}: ours worst-angle={o_ang:.0f}° cv={o_cv:.2f} (reference unavailable)")

        c.render_panels(panels, titles, os.path.join(c.OUTPUT_DIR, f"10_vs_{name}.png"),
                        suptitle=f"{name}: CyberRemesher vs QuadriFlow (both pure-quad)")


if __name__ == "__main__":
    main()
