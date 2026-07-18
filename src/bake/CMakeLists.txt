add_library(cyber_bake STATIC
    src/bake.cpp
)
add_library(cyber::bake ALIAS cyber_bake)

target_include_directories(cyber_bake PUBLIC include)
# Ray casting dispatches through the compute-acceleration layer (accel), which
# pulls in core; a GPU backend accelerates the bake with no call-site changes.
# The texture-to-texture Color source uses cyber::imageio LoadedImage +
# header-inline sampleBilinear (<cyber/imageio/load.hpp>), so bake needs
# imageio's include dir but NOT a link (no dependency cycle with imageio, which
# does link bake for its saveImage overloads).
# --- historical note (superseded; kept for context) ---
# cyber_imageio provides texture sampling (LoadedImage/sampleBilinear) for the
# texture-to-texture Color source. NOTE: cyber_imageio's saveImage() convenience
# layer includes bake.hpp, so cyber_imageio also links cyber_bake — this is an
# intentional static-library cycle (load.hpp is standalone; no header cycle).
target_link_libraries(cyber_bake PUBLIC cyber_core cyber_accel)
# Header-only use of <cyber/imageio/load.hpp> (LoadedImage + inline
# sampleBilinear) — take imageio's include dir without a link, so no cycle.
target_include_directories(cyber_bake PUBLIC
    $<TARGET_PROPERTY:cyber_imageio,INTERFACE_INCLUDE_DIRECTORIES>)

cyber_set_warnings(cyber_bake)
