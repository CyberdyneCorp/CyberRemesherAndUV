add_library(cyber_app STATIC
    src/document.cpp
    src/undo.cpp
    src/input.cpp
)
add_library(cyber::app ALIAS cyber_app)

target_include_directories(cyber_app PUBLIC include)
# The application-shell library layer is platform-independent: it only needs the
# mesh kernel and the canonical remesh parameters (both in core). GUI/windowing
# lives in the native desktop shell (apps/desktop), which links this library.
target_link_libraries(cyber_app PUBLIC cyber_core)

cyber_set_warnings(cyber_app)
