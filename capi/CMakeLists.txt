# C ABI facade over the pipeline (capi module, task 13.1). Built as a STATIC
# library for in-tree consumers and, optionally, as a SHARED library that the
# Python/Swift/C# bindings load at runtime.

# Static library: the canonical target other C++ code links.
add_library(cyber_capi STATIC
    src/capi.cpp
)
add_library(cyber::capi ALIAS cyber_capi)

target_include_directories(cyber_capi PUBLIC include)
target_link_libraries(cyber_capi PUBLIC cyber_core cyber_bake cyber_imageio)

# The C-facing version mirrors the CMake project() version.
target_compile_definitions(cyber_capi PRIVATE
    CYBER_CAPI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CYBER_CAPI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CYBER_CAPI_VERSION_PATCH=${PROJECT_VERSION_PATCH})

cyber_set_warnings(cyber_capi)

# Optional shared library for language bindings. The exported symbols are the
# extern "C" functions in cyber_capi.h; positions-independent code is required
# because it is linked into a .so/.dylib.
option(CYBER_BUILD_CAPI_SHARED "Build the shared cyber_capi library for bindings" ON)
if(CYBER_BUILD_CAPI_SHARED)
    add_library(cyber_capi_shared SHARED
        src/capi.cpp
    )
    target_include_directories(cyber_capi_shared PUBLIC include)
    target_link_libraries(cyber_capi_shared PRIVATE cyber_core cyber_bake cyber_imageio)
    set_target_properties(cyber_capi_shared PROPERTIES
        OUTPUT_NAME cyber_capi
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    target_compile_definitions(cyber_capi_shared PRIVATE
        CYBER_CAPI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        CYBER_CAPI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        CYBER_CAPI_VERSION_PATCH=${PROJECT_VERSION_PATCH})
    cyber_set_warnings(cyber_capi_shared)
endif()
