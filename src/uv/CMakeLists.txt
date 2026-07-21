add_library(cyber_uv STATIC
    src/unwrap.cpp
    src/packing.cpp
    src/atlas.cpp
)
add_library(cyber::uv ALIAS cyber_uv)

target_include_directories(cyber_uv PUBLIC include)
target_link_libraries(cyber_uv PUBLIC cyber_core)

cyber_set_warnings(cyber_uv)
