add_library(cyber_imageio STATIC
    src/png.cpp
    src/exr.cpp
    src/zip.cpp
    src/image.cpp
)
add_library(cyber::imageio ALIAS cyber_imageio)

target_include_directories(cyber_imageio PUBLIC include)
# Convenience saveImage() overloads consume cyber::bake::Image; bake pulls in core.
target_link_libraries(cyber_imageio PUBLIC cyber_bake cyber_core)

cyber_set_warnings(cyber_imageio)
