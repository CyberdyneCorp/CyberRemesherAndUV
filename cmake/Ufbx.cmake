# ufbx — the vendored FBX reader behind mesh-io's `.fbx` import.
#
# ufbx ships as one C translation unit (thirdparty/ufbx/ufbx.c, ~1.1 MB). It is
# built into an ISOLATED static library so it never inherits the tree's strict
# C++20 warning set: it is C99 third-party code and cyber_set_warnings is never
# called on it, exactly as cyber_quadcover_solver isolates Geogram.
function(cyber_add_ufbx)
    set(_dir "${PROJECT_SOURCE_DIR}/thirdparty/ufbx")

    add_library(cyber_ufbx STATIC "${_dir}/ufbx.c")
    set_target_properties(cyber_ufbx PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    # SYSTEM so ufbx.h stops -Wold-style-cast / -Wconversion from firing inside
    # the C++ importer that includes it.
    target_include_directories(cyber_ufbx SYSTEM PUBLIC "${_dir}")

    # The importer reads geometry and nothing else. Trimming the optional
    # subsystems (implementation-only switches; ufbx.h is unaffected, so the
    # header stays the same for every consumer) keeps this translation unit
    # from compiling code no caller can reach.
    target_compile_definitions(cyber_ufbx PRIVATE
        UFBX_NO_SUBDIVISION          # we subdivide with Mesh::linearSubdivide
        UFBX_NO_TESSELLATION         # NURBS/patch tessellation
        UFBX_NO_GEOMETRY_CACHE
        UFBX_NO_SCENE_EVALUATION     # animation playback
        UFBX_NO_SKINNING_EVALUATION
        UFBX_NO_ANIMATION_BAKING
        UFBX_NO_TRIANGULATION        # we triangulate with Mesh::triangulateFace
        UFBX_NO_INDEX_GENERATION
        UFBX_NO_FORMAT_OBJ)          # OBJ is tinyobjloader's job

    if(MSVC)
        target_compile_options(cyber_ufbx PRIVATE /w)
    else()
        target_compile_options(cyber_ufbx PRIVATE -w)
        # ufbx calls into libm (sqrt/pow/fmod); on Linux that is a separate library.
        target_link_libraries(cyber_ufbx PUBLIC m)
    endif()
endfunction()
