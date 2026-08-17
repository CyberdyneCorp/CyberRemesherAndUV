# Floating-point evaluation rules, applied to EVERY target in the tree
# (project code, vendored thirdparty and the vendored QuadCover solver alike) —
# a value that crosses a module boundary is only reproducible if both sides
# rounded it the same way.
#
# WHY: C and C++ let a compiler contract `a * b + c` into a single fused
# multiply-add, which rounds ONCE instead of twice. Whether it does is purely a
# property of the target ISA, so the same source produces different bits per
# platform:
#
#   * aarch64 (iPad, Android, Apple silicon) has FMADD/FMSUB in its BASELINE
#     ISA, so GCC/Clang contract by default at -O2 and above.
#   * x86-64 has no FMA in its baseline, so a stock desktop/CI build never
#     contracts — and therefore never sees the divergence. Build the same
#     sources with -march=native (or any -mfma / -mavx2 flag) and x86-64
#     reproduces the arm64 pixels exactly.
#
# The engine's bit-identity gates (the bake pixel checksums, the remesh mesh
# goldens) are written against the non-contracted rounding, and the difference
# is not cosmetic: contraction changes which side of zero a difference of
# products lands on, so the rasterizer's inside test and the ray/triangle
# determinant flip verdict and a bake loses covered texels outright.
#
# Contraction buys nothing measurable here — the hot paths are BVH traversal and
# memory bound, and an A/B on an FMA-capable build moved neither a bake nor a
# 20k-quad remesh outside run-to-run noise — so determinism wins.
function(cyber_apply_fp_rules)
    if(MSVC)
        # MSVC exposes no contraction-only switch: /fp:strict would also pin
        # exception and rounding-mode semantics, which costs far more than the
        # contraction it disables. The bit-identity gates therefore run on the
        # GCC/Clang lanes, which is every shipping target (iOS, Android, Linux,
        # macOS) plus CI.
        return()
    endif()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>)
endfunction()
