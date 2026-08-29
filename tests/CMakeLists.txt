add_library(cyber_doctest INTERFACE)
target_include_directories(cyber_doctest SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/thirdparty/doctest)

add_executable(cyber_tests
    main.cpp
    bake/test_bake.cpp
    bake/test_bake_determinism.cpp
    bake/test_curvature.cpp
    bake/test_tangent.cpp
    bake/test_ao_projected.cpp
    bake/test_field_bake.cpp
    handoff/test_handoff.cpp
    quadrangulate/test_crossfield.cpp
    quadrangulate/test_field_quadrangulator.cpp
    quadrangulate/test_position_field.cpp
    quadrangulate/test_quadcover_extractor.cpp
    quadrangulate/test_seamless_solver.cpp
    quadrangulate/test_bimdf_quantize.cpp
    quadrangulate/test_topology_layout.cpp
    quadrangulate/test_sparse_cholesky.cpp
    quadrangulate/test_stable_angle.cpp
    quadrangulate/test_quad_cleanup.cpp
    quadrangulate/test_mcf_edgediff.cpp
    quadrangulate/test_flow_guides.cpp
    core/test_bvh.cpp
    core/test_golden.cpp
    core/test_golden_organic.cpp
    core/test_holes.cpp
    core/test_export_preset.cpp
    core/test_io_fbx.cpp
    core/test_io_formats.cpp
    core/test_io_obj.cpp
    core/test_isotropic.cpp
    core/test_parallel_chunks.cpp
    core/test_pipeline.cpp
    core/test_pure_quad_quality.cpp
    core/test_reference_surface.cpp
    core/test_remesh_params.cpp
    core/test_flow_guides_pipeline.cpp
    core/test_mesh.cpp
    core/test_mesh_property.cpp
    core/test_threading.cpp
    core/test_version.cpp
    cli/test_parse_number.cpp
    accel/test_cpu_backend.cpp
    accel/test_gpu_parity.cpp
    accel/test_gpu_geometry.cpp
    accel/test_primitives.cpp
)
target_link_libraries(cyber_tests PRIVATE cyber_accel cyber_bake cyber_handoff
                                          cyber_quadrangulate cyber_doctest)
# cli/test_parse_number.cpp includes the CLI's header-only number parser. It is
# NOT gated on CYBER_BUILD_CLI: the parser's fallback path exists for the mobile
# presets, which are exactly the ones that build no CLI.
target_include_directories(cyber_tests PRIVATE ${PROJECT_SOURCE_DIR}/apps/cli)
# tests/support/: header-only helpers shared across the suite (portable setenv).
target_include_directories(cyber_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
# Committed binary fixtures the suite cannot author itself (FBX has no
# permissively licensed writer) — see tests/data/generate_fbx_fixtures.py.
target_compile_definitions(cyber_tests PRIVATE
    CYBER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
if(TARGET cyber_net)
    target_sources(cyber_tests PRIVATE net/test_bridge.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_net)
endif()
# Batch-authored modules: their tests build only when the module is enabled.
if(TARGET cyber_imageio)
    # test_load: imageio PNG reader; test_texture_bake: bake's texture color
    # source (uses imageio's LoadedImage) — both need cyber_imageio linked.
    target_sources(cyber_tests PRIVATE imageio/test_imageio.cpp imageio/test_load.cpp
                                        imageio/test_load_general.cpp bake/test_texture_bake.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_imageio)
endif()
if(TARGET cyber_capi)
    target_sources(cyber_tests PRIVATE capi/test_capi.cpp capi/test_capi_cache.cpp
                                        capi/test_capi_isotropic.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_capi)
    # Some ABI promises live only in the header's prose (handle lifetimes), so
    # the capi cases read it and gate on what it says.
    target_compile_definitions(cyber_tests PRIVATE
        CYBER_CAPI_HEADER_PATH="${PROJECT_SOURCE_DIR}/capi/include/cyber_capi.h")
endif()
if(TARGET cyber_app)
    target_sources(cyber_tests PRIVATE app/test_app.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_app)
    # The C ABI's cyber_document_* handle is implemented only when this module
    # is in the build, so the matching capi cases are gated on it.
    target_compile_definitions(cyber_tests PRIVATE CYBER_TESTS_HAVE_APP)
endif()
if(TARGET cyber_uv)
    target_sources(cyber_tests PRIVATE uv/test_uv.cpp uv/test_uv_clone.cpp
                                        uv/test_seam_path.cpp
                                        uv/test_rejection_memo.cpp
                                        handoff/test_handoff_pipeline.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_uv)
endif()
if(TARGET cyber_exportbundle)
    target_sources(cyber_tests PRIVATE exportbundle/test_bundle.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_exportbundle)
    # The C ABI's preset DATA half is testable everywhere; only its bundle
    # WRITER needs this module, so the matching capi cases are gated on it.
    target_compile_definitions(cyber_tests PRIVATE CYBER_TESTS_HAVE_EXPORTBUNDLE)
endif()
if(TARGET cyber_render)
    target_sources(cyber_tests PRIVATE render/test_camera.cpp render/test_mesh_stream.cpp
                                        render/test_overlays.cpp render/test_stage_preview.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_render)
endif()
if(TARGET cyber_retopo)
    target_sources(cyber_tests PRIVATE retopo/test_retopo.cpp retopo/test_retopo_advanced.cpp
                                        retopo/test_soft_selection.cpp retopo/test_conform.cpp
                                        retopo/test_adjacency.cpp
                                        retopo/test_stroke_interpreter.cpp
                                        retopo/test_subdivision.cpp
                                        retopo/test_vertex_snap.cpp
                                        retopo/test_loop_subdivide.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_retopo)
endif()
if(TARGET cyber_bakecage)
    target_sources(cyber_tests PRIVATE bakecage/test_bakecage.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_bakecage)
endif()
# The vendored Geogram field solver owns the process-global state the host-hygiene
# cases guard, so the one that names a Geogram symbol compiles only when that
# solver is actually in the link.
if(TARGET cyber_quadcover_solver)
    target_compile_definitions(cyber_tests PRIVATE CYBER_TESTS_HAVE_QUADCOVER)
endif()
# Min-cost-flow integer layout (M1): the SciPP-linkage self-test builds only when
# the vendored SciPP is enabled (cyber_quadrangulate carries scipp::scipp then).
if(CYBER_WITH_SCIPP)
    target_sources(cyber_tests PRIVATE quadrangulate/test_mcf_layout.cpp
                                        quadrangulate/test_mcf_flow.cpp)
endif()
cyber_set_warnings(cyber_tests)

add_test(NAME unit COMMAND cyber_tests)

# Fuzz harnesses over the untrusted-input surface, plus the replay of their
# checked-in seed corpus (tests/fuzz). The campaigns that hardened these parsers
# left no harness and no corpus behind, so nothing they found could regress-test
# anything. The replay binary needs no clang and no libFuzzer, runs everywhere
# and is bounded, so it is an ordinary ctest case; a real campaign is
# -DCYBER_BUILD_FUZZERS=ON with clang (see tests/fuzz/README.md).
if(TARGET cyber_imageio)
    add_executable(cyber_fuzz_replay
        fuzz/replay_main.cpp
        fuzz/temp_input.cpp
        fuzz/fuzz_png.cpp
        fuzz/fuzz_mesh_io.cpp)
    target_include_directories(cyber_fuzz_replay PRIVATE fuzz)
    target_link_libraries(cyber_fuzz_replay PRIVATE cyber_core cyber_imageio)
    cyber_set_warnings(cyber_fuzz_replay)

    add_test(NAME fuzz_corpus_replay
        COMMAND cyber_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus)
    set_tests_properties(fuzz_corpus_replay PROPERTIES TIMEOUT 120)

    option(CYBER_BUILD_FUZZERS "Build the libFuzzer targets (needs clang)" OFF)
    if(CYBER_BUILD_FUZZERS)
        foreach(_target png meshIo)
            add_executable(cyber_fuzz_${_target}
                fuzz/libfuzzer_entry.cpp
                fuzz/temp_input.cpp
                fuzz/fuzz_png.cpp
                fuzz/fuzz_mesh_io.cpp)
            target_include_directories(cyber_fuzz_${_target} PRIVATE fuzz)
            target_compile_definitions(cyber_fuzz_${_target} PRIVATE
                CYBER_FUZZ_TARGET=cyber::fuzz::${_target})
            target_compile_options(cyber_fuzz_${_target} PRIVATE -fsanitize=fuzzer-no-link)
            target_link_options(cyber_fuzz_${_target} PRIVATE -fsanitize=fuzzer)
            target_link_libraries(cyber_fuzz_${_target} PRIVATE cyber_core cyber_imageio)
        endforeach()
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter)

# CTest prefixes a native test COMMAND with CMAKE_CROSSCOMPILING_EMULATOR by
# itself, but the Python-driven cases launch the engine binaries THEMSELVES —
# the command CTest sees is the interpreter, which is the host's. Hand them the
# same emulator so `ctest` works on a cross-build (the arm64 lane) without
# binfmt registration, i.e. without root. Empty on a native build.
set(CYBER_TEST_LAUNCHER_ENV "")
if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
    string(REPLACE ";" " " _cyber_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
    set(CYBER_TEST_LAUNCHER_ENV "CYBER_TEST_LAUNCHER=${_cyber_emulator}")
endif()

# Single version identity (build-and-packaging spec). Pure file parsing, so it
# needs no built target and runs in every configuration that has an interpreter.
if(Python3_FOUND)
    add_test(NAME version_identity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_version_identity.py)

    # Swift package <-> C ABI parity. The SwiftPM package binds cyber_capi.h
    # directly but only a macOS runner can compile it, so it silently drifted
    # until a release lane ran. This is pure text parsing of both sides — no
    # Swift toolchain, no engine — so it guards the binding everywhere.
    add_test(NAME swift_abi_parity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_swift_abi_parity.py)

    # Objective-C++ syntax gate for the Metal compute backend. Same idea as the
    # Swift guard above: metal_backend.mm needs an Apple toolchain to build and
    # the only lane that has one runs continue-on-error, so nothing compiled it
    # at all. Clang parses Objective-C++ on any host; declaration-only
    # Metal/Foundation stubs supply the rest. Skips (CTest code 77) where no
    # suitable clang is installed.
    add_test(NAME metal_objcxx_syntax
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_metal_syntax.py)
    set_tests_properties(metal_objcxx_syntax PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

    # Build-description regressions the C++ suite cannot see: the default
    # -Werror configuration, the CAPI/retopo option dependency, and the
    # exported surface of the shared C ABI. Given the built library it also
    # checks the real symbol table; without it, the source-level checks run.
    add_test(NAME build_hygiene
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_build_hygiene.py)
    set_tests_properties(build_hygiene PROPERTIES TIMEOUT 300)
    if(TARGET cyber_capi_shared)
        set_tests_properties(build_hygiene PROPERTIES
            ENVIRONMENT "CYBER_CAPI_LIB=$<TARGET_FILE:cyber_capi_shared>")
    endif()

    # Release-lane regressions: the wheel that carried no engine, the licence gate
    # that could not see the vendored Geogram tree, the release that published
    # without running the suite, the AppImage that bundled no libraries, and the
    # missing sanitizer/fuzz machinery. All of it lived in YAML and shell, where
    # no test could reach it. Pure text/file parsing plus one exercise of the
    # wheel staging script, so it runs in every configuration.
    add_test(NAME release_gates
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/test_release_gates.py)
    set_tests_properties(release_gates PROPERTIES TIMEOUT 120)
endif()

if(CYBER_BUILD_CLI AND Python3_FOUND)
    add_test(NAME cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_cli.py
                $<TARGET_FILE:cyberremesh>)
    if(CYBER_TEST_LAUNCHER_ENV)
        set_tests_properties(cli PROPERTIES ENVIRONMENT "${CYBER_TEST_LAUNCHER_ENV}")
    endif()

    # Benchmark regression check (tools/bench) — recorded-baseline gate over the
    # deterministic generated corpus. It reports CTest's SKIP code (77) rather
    # than failing in the two cases where it cannot say anything: numpy/scipy
    # absent, and a binary carrying a different seamless-UV solver than the
    # baselines were recorded on (those numbers describe one solver; comparing
    # the other against them is not a regression report).
    #
    # Registered either way. It used to be dropped at configure time when the
    # dependencies were missing, so the case did not appear in the ctest list at
    # all — which is how nobody noticed this gate had never once run in CI.
    if(CMAKE_CROSSCOMPILING)
        # The baselines record wall-clock and quality for a NATIVE binary; the
        # same binary under an instruction emulator reports neither honestly.
        message(STATUS "bench test skipped: cross-build (timings under an emulator are not baselines)")
    else()
        add_test(NAME bench
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/bench/bench.py
                    check --cyber-binary $<TARGET_FILE:cyberremesh>
                    --cache-dir ${PROJECT_BINARY_DIR}/bench-cache)
        set_tests_properties(bench PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
    endif()
endif()

if(TARGET cyberbridge-server AND Python3_FOUND)
    add_test(NAME bridge_python
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_integration.py
                $<TARGET_FILE:cyberbridge-server>)
    if(CYBER_TEST_LAUNCHER_ENV)
        set_tests_properties(bridge_python PROPERTIES ENVIRONMENT "${CYBER_TEST_LAUNCHER_ENV}")
    endif()
endif()

# Reply-limit regression for the Python client. Drives it against a stub server
# on loopback, so it needs only an interpreter — no bridge binary.
if(Python3_FOUND)
    add_test(NAME bridge_python_limits
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_client_limits.py)
endif()

# Python ctypes bindings over the C ABI shared library. A skip (lib not loadable)
# returns CTest's SKIP code (77) so it shows as "Skipped", never a vacuous "Passed".
# To make the tests actually run — even when Python3_EXECUTABLE ships an older
# libstdc++ than the toolchain that built the lib (e.g. a conda Python) — we
# LD_PRELOAD the compiler's own libstdc++, which is a superset of what the
# interpreter needs and lets ctypes resolve the lib's newer GLIBCXX symbols.
if(TARGET cyber_capi_shared AND Python3_FOUND)
    set(_pydir ${CMAKE_CURRENT_SOURCE_DIR}/../python/cyberremesh/tests)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so.6
        OUTPUT_VARIABLE _toolchain_libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    get_filename_component(_toolchain_libstdcxx "${_toolchain_libstdcxx}" REALPATH)
    set(_py_test_env "CYBER_CAPI_LIB=$<TARGET_FILE:cyber_capi_shared>")
    set(_py_preload "${_toolchain_libstdcxx}")
    if(NOT EXISTS "${_toolchain_libstdcxx}")
        set(_py_preload "")
    endif()
    # Under AddressSanitizer the interpreter has to load the ASan runtime FIRST
    # or ctypes aborts the moment it opens the library ("ASan runtime does not
    # come first in initial library list") — which failed every one of these
    # cases on the sanitizer lane, since it runs the whole ctest set. Preload
    # the compiler's own runtime ahead of libstdc++, and turn the leak checker
    # off for the interpreter's process: CPython retains by design, and the
    # engine's own leaks are covered by the C++ suite in the same build.
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _cfg)
    set(_cxx_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_cfg}}")
    if(_cxx_flags MATCHES "-fsanitize=[A-Za-z,-]*address")
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libasan.so
            OUTPUT_VARIABLE _asan_runtime OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        get_filename_component(_asan_runtime "${_asan_runtime}" REALPATH)
        if(EXISTS "${_asan_runtime}")
            set(_py_preload "${_asan_runtime}:${_py_preload}")
            list(APPEND _py_test_env "ASAN_OPTIONS=detect_leaks=0")
        endif()
    endif()
    if(NOT _py_preload STREQUAL "")
        list(APPEND _py_test_env "LD_PRELOAD=${_py_preload}")
    endif()
    foreach(_pyt test_api test_golden test_document_invariants test_bake test_uv_atlas
                 test_hole_fill_policy test_open_surface_cleanup
                 test_cad_density_robustness test_soft_selection test_seam_path
                 test_handoff test_mesh_state test_export_presets
                 test_library_lookup test_host_process_hygiene
                 test_io_formats test_subdivide test_loop_subdivide test_examples
                 test_unwrap_seams test_retopo_ops test_isotropic_remesh
                 test_reference_nan_guard)
        add_test(NAME python_${_pyt}
            COMMAND ${Python3_EXECUTABLE} ${_pydir}/${_pyt}.py)
        set_tests_properties(python_${_pyt} PROPERTIES
            ENVIRONMENT "${_py_test_env}"
            SKIP_RETURN_CODE 77)
    endforeach()
    # test_examples runs the real pipeline across six example scripts; the
    # default 1500s CTest timeout is tight for that on a loaded CI runner.
    set_tests_properties(python_test_examples PROPERTIES TIMEOUT 1800)
endif()

# Benchmark count-matching regression tests. Pure control flow — no engine, no
# models, no network — so they need only an interpreter, and they guard the search
# that decides whether a Phase 1/3 comparison is count-matched at all. They lived
# unregistered (and therefore unrun) since they were written.
if(Python3_FOUND)
    add_test(NAME python_test_count_match
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../examples/test_count_match.py)
endif()
