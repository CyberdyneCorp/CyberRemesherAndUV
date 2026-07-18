add_library(cyber_doctest INTERFACE)
target_include_directories(cyber_doctest SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/thirdparty/doctest)

add_executable(cyber_tests
    main.cpp
    core/test_bvh.cpp
    core/test_golden.cpp
    core/test_holes.cpp
    core/test_io_formats.cpp
    core/test_io_obj.cpp
    core/test_isotropic.cpp
    core/test_pipeline.cpp
    core/test_reference_surface.cpp
    core/test_remesh_params.cpp
    core/test_mesh.cpp
    core/test_mesh_property.cpp
    core/test_version.cpp
    accel/test_cpu_backend.cpp
    accel/test_gpu_parity.cpp
    accel/test_primitives.cpp
)
target_link_libraries(cyber_tests PRIVATE cyber_accel cyber_doctest)
if(TARGET cyber_net)
    target_sources(cyber_tests PRIVATE net/test_bridge.cpp)
    target_link_libraries(cyber_tests PRIVATE cyber_net)
endif()
cyber_set_warnings(cyber_tests)

add_test(NAME unit COMMAND cyber_tests)

if(CYBER_BUILD_CLI)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME cli
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_cli.py
                    $<TARGET_FILE:cyberremesh>)
    endif()
endif()
