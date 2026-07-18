add_library(cyber_retopo STATIC
    src/snapping.cpp
    src/actions.cpp
)
add_library(cyber::retopo ALIAS cyber_retopo)

target_include_directories(cyber_retopo PUBLIC include)
# Snapping is the accelerated closest-point path over the Target BVH; all edits
# are structural operations on cyber::Mesh, so core is the only dependency.
target_link_libraries(cyber_retopo PUBLIC cyber_core)

cyber_set_warnings(cyber_retopo)
