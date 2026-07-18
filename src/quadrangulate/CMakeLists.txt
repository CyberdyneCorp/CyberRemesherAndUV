add_library(cyber_quadrangulate STATIC
    src/crossfield.cpp
    src/field_quadrangulator.cpp
)
add_library(cyber::quadrangulate ALIAS cyber_quadrangulate)

target_include_directories(cyber_quadrangulate PUBLIC include)
# The cross-field smoothing dispatches its spmv through the accel layer, so a
# GPU backend accelerates it (task 5.8).
target_link_libraries(cyber_quadrangulate PUBLIC cyber_core cyber_accel)

cyber_set_warnings(cyber_quadrangulate)
