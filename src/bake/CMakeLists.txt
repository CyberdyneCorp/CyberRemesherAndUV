add_library(cyber_bake STATIC
    src/bake.cpp
)
add_library(cyber::bake ALIAS cyber_bake)

target_include_directories(cyber_bake PUBLIC include)
# Ray casting dispatches through the compute-acceleration layer (accel), which
# pulls in core; a GPU backend accelerates the bake with no call-site changes.
target_link_libraries(cyber_bake PUBLIC cyber_core cyber_accel)

cyber_set_warnings(cyber_bake)
