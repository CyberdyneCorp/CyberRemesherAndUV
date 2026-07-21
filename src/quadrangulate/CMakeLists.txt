add_library(cyber_quadrangulate STATIC
    src/crossfield.cpp
    src/field_quadrangulator.cpp
    src/quadcover_extractor.cpp
    src/position_field.cpp
    src/quad_extract.cpp
    src/seamless_solver.cpp
    src/mcf_layout.cpp
    src/mcf_edgediff.cpp
)
add_library(cyber::quadrangulate ALIAS cyber_quadrangulate)

target_include_directories(cyber_quadrangulate PUBLIC include)
# The cross-field smoothing dispatches its spmv through the accel layer, so a
# GPU backend accelerates it (task 5.8).
target_link_libraries(cyber_quadrangulate PUBLIC cyber_core cyber_accel)

cyber_set_warnings(cyber_quadrangulate)

# When the in-process QuadCover solver is enabled, link the isolated Geogram +
# AutoRemesher static library and flip computeSeamlessUv to the in-process path
# (no CYBER_QUADCOVER_CLI subprocess / temp files). See cmake/QuadCoverSolver.cmake.
if(CYBER_WITH_QUADCOVER)
    cyber_add_quadcover_solver()
    target_link_libraries(cyber_quadrangulate PRIVATE cyber_quadcover_solver)
    target_compile_definitions(cyber_quadrangulate PRIVATE CYBER_HAVE_QUADCOVER)
endif()

# Min-cost-flow integer quadrangulator: link the vendored SciPP (scipp::scipp)
# imported by cmake/SciPP.cmake. See docs/mcf-integer-layout-plan.md.
if(CYBER_WITH_SCIPP)
    target_link_libraries(cyber_quadrangulate PRIVATE scipp::scipp)
    target_compile_definitions(cyber_quadrangulate PRIVATE CYBER_HAVE_SCIPP)
endif()
