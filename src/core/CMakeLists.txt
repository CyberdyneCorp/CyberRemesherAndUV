add_library(cyber_core STATIC
    src/bvh.cpp
    src/io.cpp
    src/io_gltf.cpp
    src/io_obj.cpp
    src/io_ply.cpp
    src/io_stl.cpp
    src/isotropic.cpp
    src/mesh.cpp
    src/mesh_diagnostics.cpp
    src/mesh_ops.cpp
    src/pipeline.cpp
    src/quadrangulate.cpp
    src/remesh_params.cpp
    src/version.cpp
)

target_include_directories(cyber_core SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/thirdparty/tinyobjloader
    ${CMAKE_SOURCE_DIR}/thirdparty/happly
    ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_library(cyber::core ALIAS cyber_core)

target_include_directories(cyber_core PUBLIC include)
target_compile_definitions(cyber_core PRIVATE
    CYBER_VERSION_STRING="${PROJECT_VERSION}")

cyber_set_warnings(cyber_core)
