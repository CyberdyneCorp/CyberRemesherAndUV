add_executable(cyberremesh
    main.cpp
)

# cyber_accel propagates cyber_core publicly.
target_link_libraries(cyberremesh PRIVATE cyber_accel)
# nlohmann json.hpp (vendored with tinygltf, MIT) for the JSON report.
target_include_directories(cyberremesh SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)

cyber_set_warnings(cyberremesh)
