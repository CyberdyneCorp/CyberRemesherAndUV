add_library(cyber_net STATIC
    src/protocol.cpp
    src/message.cpp
    src/session.cpp
    src/socket_io.cpp
    src/server.cpp
    src/client.cpp
)
add_library(cyber::net ALIAS cyber_net)

target_include_directories(cyber_net PUBLIC include)
# nlohmann/json is vendored with tinygltf (MIT, audited); included as SYSTEM so
# its internals never trip the strict project warning flags.
target_include_directories(cyber_net SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)

find_package(Threads REQUIRED)
target_link_libraries(cyber_net PUBLIC Threads::Threads)

cyber_set_warnings(cyber_net)
