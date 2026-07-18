add_library(cyber_accel STATIC
    src/cpu_backend.cpp
    src/registry.cpp
)
add_library(cyber::accel ALIAS cyber_accel)

target_include_directories(cyber_accel PUBLIC include)
target_link_libraries(cyber_accel PUBLIC cyber_core)

find_package(Threads REQUIRED)
target_link_libraries(cyber_accel PUBLIC Threads::Threads)

if(CYBER_ENABLE_METAL)
    target_compile_definitions(cyber_accel PUBLIC CYBER_HAS_METAL=1)
    # Metal backend sources land here (task group 4.3).
endif()
if(CYBER_ENABLE_CUDA)
    target_compile_definitions(cyber_accel PUBLIC CYBER_HAS_CUDA=1)
    # CUDA backend sources land here (task group 4.4).
endif()
if(CYBER_ENABLE_OPENCL)
    target_compile_definitions(cyber_accel PUBLIC CYBER_HAS_OPENCL=1)
    # OpenCL backend sources land here (task group 4.5).
endif()

cyber_set_warnings(cyber_accel)
