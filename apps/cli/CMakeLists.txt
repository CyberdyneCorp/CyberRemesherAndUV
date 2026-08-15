add_executable(cyberremesh
    main.cpp
)

# cyber_accel propagates cyber_core publicly; cyber_quadrangulate provides the
# field-aligned quadrangulator the CLI injects.
# cyber_handoff provides --target (the versioned sculpt handoff reader).
target_link_libraries(cyberremesh PRIVATE cyber_accel cyber_quadrangulate cyber_handoff)
# --preset packages the mesh plus its baked maps. The bundle module rides on
# CYBER_BUILD_UV (it unwraps a low-poly that has no UVs); without it the flag
# reports that this build cannot honor presets rather than silently skipping.
if(TARGET cyber_exportbundle)
    target_link_libraries(cyberremesh PRIVATE cyber_exportbundle)
    target_compile_definitions(cyberremesh PRIVATE CYBER_CLI_HAVE_PRESETS=1)
endif()
# nlohmann json.hpp (vendored with tinygltf, MIT) for the JSON report.
target_include_directories(cyberremesh SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/thirdparty/tinygltf)

cyber_set_warnings(cyberremesh)
