add_library(cyber_render STATIC
    src/rhi.cpp
    src/mesh_stream.cpp
    src/overlays.cpp
    src/stage_preview.cpp
    src/camera.cpp
)
add_library(cyber::render ALIAS cyber_render)

target_include_directories(cyber_render PUBLIC include)
target_link_libraries(cyber_render PUBLIC cyber_core)

cyber_set_warnings(cyber_render)

# GPU backends compile in isolated targets WITHOUT the strict project warning
# flags: vendor headers (Metal, vulkan/vulkan.h) do not survive -Werror
# -Wold-style-cast -Wconversion. Each maker returns null when its device is
# absent, so linking them in never forces a GPU at runtime. Both are OFF by
# default and unbuildable in headless CI (no windowing/SDK).

option(CYBER_RENDER_ENABLE_VULKAN "Build the Vulkan RHI backend (needs Vulkan SDK)" OFF)
option(CYBER_RENDER_ENABLE_METAL "Build the Metal RHI backend (Apple only)" OFF)

if(CYBER_RENDER_ENABLE_VULKAN)
    # UNVERIFIED: requires the Vulkan SDK + a real GPU/window.
    find_package(Vulkan REQUIRED)
    add_library(cyber_render_vulkan STATIC src/vulkan_backend.cpp)
    target_link_libraries(cyber_render_vulkan PUBLIC cyber_render Vulkan::Vulkan)
    target_link_libraries(cyber_render PUBLIC cyber_render_vulkan)
    target_compile_definitions(cyber_render PRIVATE CYBER_RENDER_HAS_VULKAN=1)
endif()

if(CYBER_RENDER_ENABLE_METAL)
    # UNVERIFIED on non-Apple hosts; Objective-C++ + Metal.framework.
    enable_language(OBJCXX)
    add_library(cyber_render_metal STATIC src/metal_backend.mm)
    target_link_libraries(cyber_render_metal PUBLIC cyber_render)
    target_link_libraries(cyber_render_metal PUBLIC "-framework Metal" "-framework QuartzCore"
                          "-framework Foundation")
    target_link_libraries(cyber_render PUBLIC cyber_render_metal)
    target_compile_definitions(cyber_render PRIVATE CYBER_RENDER_HAS_METAL=1)
endif()
