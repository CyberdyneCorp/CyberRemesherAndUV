add_library(cyber_accel STATIC
    src/backend_primitives.cpp
    src/cpu_backend.cpp
    src/registry.cpp
)
add_library(cyber::accel ALIAS cyber_accel)

target_include_directories(cyber_accel PUBLIC include)
target_link_libraries(cyber_accel PUBLIC cyber_core)

find_package(Threads REQUIRED)
target_link_libraries(cyber_accel PUBLIC Threads::Threads)

cyber_set_warnings(cyber_accel)

# GPU backends compile in isolated targets WITHOUT the strict project warning
# flags: vendor headers (CL/opencl.hpp, cuda_runtime.h, Metal) do not survive
# -Werror -Wold-style-cast -Wconversion. Each maker returns null when its
# device is absent, so linking them in never forces a GPU at runtime.

if(CYBER_ENABLE_METAL)
    # UNVERIFIED on non-Apple hosts; Objective-C++ + Metal.framework.
    add_library(cyber_accel_metal STATIC src/metal_backend.mm)
    target_link_libraries(cyber_accel_metal PUBLIC cyber_accel)
    target_link_libraries(cyber_accel_metal PUBLIC "-framework Metal" "-framework Foundation")
    target_link_libraries(cyber_accel PUBLIC cyber_accel_metal)
    target_compile_definitions(cyber_accel PRIVATE CYBER_HAS_METAL=1)
endif()

if(CYBER_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    # Virtual (PTX) arch: the driver JITs for the actual GPU at load, so one
    # build runs on GPUs newer than the toolkit (e.g. sm_120 on CUDA 12.0).
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 90-virtual)
    endif()
    add_library(cyber_accel_cuda STATIC src/cuda_backend.cu)
    set_target_properties(cyber_accel_cuda PROPERTIES CUDA_STANDARD 20)
    target_link_libraries(cyber_accel_cuda PUBLIC cyber_accel CUDA::cudart)
    target_link_libraries(cyber_accel PUBLIC cyber_accel_cuda)
    target_compile_definitions(cyber_accel PRIVATE CYBER_HAS_CUDA=1)
endif()

if(CYBER_ENABLE_OPENCL)
    find_package(OpenCL REQUIRED)
    add_library(cyber_accel_opencl STATIC src/opencl_backend.cpp)
    target_link_libraries(cyber_accel_opencl PUBLIC cyber_accel OpenCL::OpenCL)
    target_link_libraries(cyber_accel PUBLIC cyber_accel_opencl)
    target_compile_definitions(cyber_accel PRIVATE CYBER_HAS_OPENCL=1)
endif()
