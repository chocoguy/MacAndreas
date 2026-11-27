cmake_policy(SET CMP0157 NEW)
enable_language(Swift)
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")

add_executable(MacAndreas MACOSX_BUNDLE
    Sources/LauncherApp.swift
    Sources/ContentView.swift
    Sources/Model.swift
    Sources/Config.swift
    Sources/Paths.swift
    Sources/Tools.swift
    Sources/Stamp.swift
    Sources/Displays.swift
    Sources/Headless.swift
    Sources/ModSettings.swift
)
set_target_properties(MacAndreas PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "MacAndreas"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.macandreas.launcher"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    Swift_LANGUAGE_VERSION 5
    Swift_COMPILATION_MODE wholemodule)
target_compile_options(MacAndreas PRIVATE $<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>)
target_link_libraries(MacAndreas PRIVATE "-framework SwiftUI" "-framework AppKit" "-framework CryptoKit")

set(MA_BUNDLE_ARGS
    -DBUNDLE=$<TARGET_BUNDLE_DIR:MacAndreas>
    -DSA_HOST=$<TARGET_FILE:sa-host>
    -DSA_VERIFY=$<TARGET_FILE:sa-verify>
    -DMANIFEST_DIR=${CMAKE_SOURCE_DIR}/manifest
    -DMOD_SETTINGS_SCHEMA=${CMAKE_SOURCE_DIR}/mods/settings.yaml
    -DICON=${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png
    -DANALYSIS_DIR=${CMAKE_SOURCE_DIR}/analysis/sa-1.0us)
set(MA_BUNDLE_DEPENDS MacAndreas sa-host sa-verify)
if(TARGET sa-recomp)
    list(APPEND MA_BUNDLE_DEPENDS sa-recomp ma_guest_intrinsics)
    list(APPEND MA_BUNDLE_ARGS
        -DSA_RECOMP=$<TARGET_FILE:sa-recomp>
        -DINTRINSICS=${MA_GUEST_INTRINSICS_BC})
endif()
add_custom_target(MacAndreas-bundle ALL
    COMMAND ${CMAKE_COMMAND} ${MA_BUNDLE_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/bundle.cmake
    COMMENT "Assembling MacAndreas.app"
    VERBATIM)
add_dependencies(MacAndreas-bundle ${MA_BUNDLE_DEPENDS})
