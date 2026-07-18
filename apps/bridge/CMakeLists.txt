add_executable(cyberbridge-server main.cpp)
target_link_libraries(cyberbridge-server PRIVATE cyber_net)
cyber_set_warnings(cyberbridge-server)
