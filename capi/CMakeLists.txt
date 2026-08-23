# C ABI facade over the pipeline (capi module, task 13.1). Built as a STATIC
# library for in-tree consumers and, optionally, as a SHARED library that the
# Python/Swift/C# bindings load at runtime.

# Static library: the canonical target other C++ code links.
add_library(cyber_capi STATIC
    src/capi.cpp
)
add_library(cyber::capi ALIAS cyber_capi)

target_include_directories(cyber_capi PUBLIC include)
# cyber_accel is named explicitly (not just inherited through cyber_quadrangulate)
# because cyber_set_backend / cyber_available_backends call into it directly.
target_link_libraries(cyber_capi PUBLIC cyber_accel cyber_core cyber_bake cyber_handoff
                                       cyber_imageio cyber_quadrangulate cyber_retopo)

# The UV atlas entry points (cyber_uv_atlas) are compiled in only when the UV
# module is built; the symbols are always declared so the ABI stays stable.
if(TARGET cyber_uv)
    target_link_libraries(cyber_capi PUBLIC cyber_uv)
    target_compile_definitions(cyber_capi PRIVATE CYBER_CAPI_WITH_UV)
endif()

# The C document handle (cyber_document_*) is the persistence seam for the
# named soft-selection slots. Same pattern as UV: declared always, implemented
# only when the application-shell library is in the build.
if(TARGET cyber_app)
    target_link_libraries(cyber_capi PUBLIC cyber_app)
    target_compile_definitions(cyber_capi PRIVATE CYBER_CAPI_WITH_APP)
endif()

# Export presets are pure data in cyber_core, so listing/resolving/reading one
# works in every configuration. Only cyber_export_bundle_write needs the bundle
# writer, which rides on CYBER_BUILD_UV; without it that call reports that this
# build cannot write bundles rather than silently doing nothing.
if(TARGET cyber_exportbundle)
    target_link_libraries(cyber_capi PUBLIC cyber_exportbundle)
    target_compile_definitions(cyber_capi PRIVATE CYBER_CAPI_WITH_EXPORTBUNDLE)
endif()

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
    include(GNUInstallDirs)
    target_include_directories(cyber_capi_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(cyber_capi_shared PRIVATE cyber_accel cyber_core cyber_bake cyber_handoff
                                              cyber_imageio cyber_quadrangulate cyber_retopo)
    if(TARGET cyber_uv)
        target_link_libraries(cyber_capi_shared PRIVATE cyber_uv)
        target_compile_definitions(cyber_capi_shared PRIVATE CYBER_CAPI_WITH_UV)
    endif()
    if(TARGET cyber_app)
        target_link_libraries(cyber_capi_shared PRIVATE cyber_app)
        target_compile_definitions(cyber_capi_shared PRIVATE CYBER_CAPI_WITH_APP)
    endif()
    if(TARGET cyber_exportbundle)
        target_link_libraries(cyber_capi_shared PRIVATE cyber_exportbundle)
        target_compile_definitions(cyber_capi_shared PRIVATE CYBER_CAPI_WITH_EXPORTBUNDLE)
    endif()
    set_target_properties(cyber_capi_shared PROPERTIES
        OUTPUT_NAME cyber_capi
        EXPORT_NAME capi           # find_package(CyberRemesher) -> cyber::capi
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    target_compile_definitions(cyber_capi_shared PRIVATE
        CYBER_CAPI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        CYBER_CAPI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        CYBER_CAPI_VERSION_PATCH=${PROJECT_VERSION_PATCH})
    cyber_set_warnings(cyber_capi_shared)

    # Export only the cyber_* C ABI. Without this the ~4000 vendored
    # Geogram/stb/tinygltf definitions linked in from the static archives stay
    # globally visible and are subject to ELF interposition inside a host
    # process (a DCC) that carries its own copy of the same third-party code.
    #
    # Windows is the third platform and had no export control at all: MSVC
    # exports nothing without __declspec(dllexport), so cyber_capi.dll came out
    # with an empty export table and produced no import library — the install
    # step then had no cyber_capi.lib and a C consumer got unresolved externals.
    # WINDOWS_EXPORT_ALL_SYMBOLS restores exactly what the shipped MinGW lane
    # already relies on (its linker auto-exports). The narrow export list the
    # other two platforms use is not needed here: PE resolves imports per-DLL,
    # so a host process carrying its own Geogram cannot interpose on ours the
    # way ELF's flat namespace allows.
    if(WIN32)
        set_target_properties(cyber_capi_shared PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
    elseif(APPLE)
        target_link_options(cyber_capi_shared PRIVATE
            "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cyber_capi.symbols")
        set_property(TARGET cyber_capi_shared APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cyber_capi.symbols")
    elseif(UNIX)
        target_link_options(cyber_capi_shared PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/cyber_capi.map"
            "-Wl,--exclude-libs,ALL")
        set_property(TARGET cyber_capi_shared APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cyber_capi.map")
    endif()

    # Install the self-contained C ABI + header so downstream projects can
    # find_package(CyberRemesher CONFIG) and link cyber::capi. (The C++ core and
    # the optional Geogram solver are PRIVATE-linked into the .so, so the export
    # is self-contained — no transitive target exports needed.)
    install(TARGETS cyber_capi_shared
        EXPORT CyberRemesherTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/cyber_capi.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
