add_library(cyber_handoff STATIC
    src/handoff.cpp
)
add_library(cyber::handoff ALIAS cyber_handoff)

target_include_directories(cyber_handoff PUBLIC include)
# The handoff is an interchange, not a coupling: core (Mesh + the typed io error
# model + the glTF importer) is the only dependency, so no consumer of this
# module inherits a dependency on any sculpting engine.
target_link_libraries(cyber_handoff PUBLIC cyber_core)
target_include_directories(cyber_handoff SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/thirdparty/happly)

cyber_set_warnings(cyber_handoff)
