add_library(cyber_doctest INTERFACE)
target_include_directories(cyber_doctest SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/thirdparty/doctest)

add_executable(cyber_tests
    main.cpp
    bake/test_bake.cpp
    bake/test_tangent.cpp
    bake/test_ao_projected.cpp
    quadrangulate/test_crossfield.cpp
    quadrangulate/test_field_quadrangulator.cpp
    quadrangulate/test_position_field.cpp
    quadrangulate/test_quadcover_extractor.cpp
    quadrangulate/test_seamless_solver.cpp
    quadrangulate/test_quad_cleanup.cpp
    quadrangulate/test_mcf_edgediff.cpp
    core/test_bvh.cpp
    core/test_golden.cpp
    core/test_golden_organic.cpp
    core/test_holes.cpp
    core/test_io_formats.cpp
    core/test_io_obj.cpp
    core/test_isotropic.cpp
    core/test_pipeline.cpp
    core/test_pure_quad_quality.cpp
    core/test_reference_surface.cpp
    core/test_remesh_params.cpp
    core/test_mesh.cpp
    core/test_mesh_property.cpp
    core/test_version.cpp
    accel/test_cpu_backend.cpp
    accel/test_gpu_parity.cpp
    accel/test_gpu_geometry.cpp
    accel/test_primitives.cpp
)
target_link_libraries(cyber_tests PRIVATE cyber_accel cyber_bake cyber_quadrangulate cyber_doctest)
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
    target_sources(cyber_tests PRIVATE capi/test_capi.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_capi)
endif()
if(TARGET cyber_app)
    target_sources(cyber_tests PRIVATE app/test_app.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_app)
endif()
if(TARGET cyber_uv)
    target_sources(cyber_tests PRIVATE uv/test_uv.cpp uv/test_uv_clone.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_uv)
endif()
if(TARGET cyber_retopo)
    target_sources(cyber_tests PRIVATE retopo/test_retopo.cpp retopo/test_retopo_advanced.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_retopo)
endif()
if(TARGET cyber_bakecage)
    target_sources(cyber_tests PRIVATE bakecage/test_bakecage.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_bakecage)
endif()
# Min-cost-flow integer layout (M1): the SciPP-linkage self-test builds only when
# the vendored SciPP is enabled (cyber_quadrangulate carries scipp::scipp then).
if(CYBER_WITH_SCIPP)
    target_sources(cyber_tests PRIVATE quadrangulate/test_mcf_layout.cpp
                                        quadrangulate/test_mcf_flow.cpp)
endif()
cyber_set_warnings(cyber_tests)

add_test(NAME unit COMMAND cyber_tests)

if(CYBER_BUILD_CLI OR TARGET cyberbridge-server OR TARGET cyber_capi_shared)
    find_package(Python3 COMPONENTS Interpreter)
endif()

if(CYBER_BUILD_CLI AND Python3_FOUND)
    add_test(NAME cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_cli.py
                $<TARGET_FILE:cyberremesh>)
endif()

if(TARGET cyberbridge-server AND Python3_FOUND)
    add_test(NAME bridge_python
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_integration.py
                $<TARGET_FILE:cyberbridge-server>)
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
    if(EXISTS "${_toolchain_libstdcxx}")
        list(APPEND _py_test_env "LD_PRELOAD=${_toolchain_libstdcxx}")
    endif()
    foreach(_pyt test_api test_golden test_document_invariants test_bake)
        add_test(NAME python_${_pyt}
            COMMAND ${Python3_EXECUTABLE} ${_pydir}/${_pyt}.py)
        set_tests_properties(python_${_pyt} PROPERTIES
            ENVIRONMENT "${_py_test_env}"
            SKIP_RETURN_CODE 77)
    endforeach()
endif()
