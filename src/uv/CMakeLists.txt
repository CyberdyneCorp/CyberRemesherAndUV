add_library(cyber_uv STATIC
    src/unwrap.cpp
    src/packing.cpp
    src/atlas.cpp
    src/seam_path.cpp
)
add_library(cyber::uv ALIAS cyber_uv)

target_include_directories(cyber_uv PUBLIC include)
target_link_libraries(cyber_uv PUBLIC cyber_core)
# Seam-path routing reuses retopo's header-only Dijkstra
# (<cyber/retopo/paths.hpp>) and vertex picking (<cyber/retopo/picking.hpp>).
# Take retopo's include dir WITHOUT a link: both headers are header-only and
# core-only, src/uv is configured BEFORE src/retopo, and CYBER_BUILD_RETOPO can
# be OFF — the same precedent as cyber_bake's use of <cyber/imageio/load.hpp>.
target_include_directories(cyber_uv PUBLIC ${PROJECT_SOURCE_DIR}/src/retopo/include)

cyber_set_warnings(cyber_uv)
