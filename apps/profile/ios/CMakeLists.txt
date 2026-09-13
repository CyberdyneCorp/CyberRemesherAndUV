enable_language(OBJCXX)

add_executable(cyber_ios_memory_profile MACOSX_BUNDLE MemoryProfile.mm)
target_link_libraries(cyber_ios_memory_profile PRIVATE cyber_capi)
target_link_libraries(cyber_ios_memory_profile PRIVATE "-framework UIKit")
target_include_directories(cyber_ios_memory_profile PRIVATE ${CMAKE_SOURCE_DIR}/capi/include)
set_target_properties(cyber_ios_memory_profile PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.cyberdynecorp.CyberRemesherMemoryProfile"
    MACOSX_BUNDLE_BUNDLE_NAME "CyberRemesher Memory Profile"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.cyberdynecorp.CyberRemesherMemoryProfile"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
