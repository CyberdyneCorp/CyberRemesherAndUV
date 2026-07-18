add_library(cyber_bakecage STATIC
    src/cage.cpp
    src/links.cpp
)
add_library(cyber::bakecage ALIAS cyber_bakecage)

target_include_directories(cyber_bakecage PUBLIC include)
# The editable projection cage and component-link model operate on the mesh
# kernel (Mesh, islands) and the BVH used for nearest-surface fallback, both in
# cyber_core. Preview descriptors are header-only and pull in nothing extra.
target_link_libraries(cyber_bakecage PUBLIC cyber_core)

cyber_set_warnings(cyber_bakecage)
