add_library(cyber_exportbundle STATIC
    src/bundle.cpp
)
add_library(cyber::exportbundle ALIAS cyber_exportbundle)

target_include_directories(cyber_exportbundle PUBLIC include)
# Preset data + mesh export come from core; the bundle bakes each map (bake),
# writes it (imageio) and unwraps the low-poly when it has no UVs (uv).
target_link_libraries(cyber_exportbundle PUBLIC cyber_core cyber_bake cyber_imageio cyber_uv)

cyber_set_warnings(cyber_exportbundle)
