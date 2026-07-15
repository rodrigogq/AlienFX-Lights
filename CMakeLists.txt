# AlienFX Lights — cross-platform build (Windows tray app + macOS menu bar app)
# from one shared C++ core. This is the whole build; open this folder directly
# in VS Code (CMake Tools), Visual Studio ("Open Folder"), or generate an Xcode
# project with `cmake -G Xcode`.
#
# Build:    cmake -B build && cmake --build build --config Release
# Package:  cmake --build build --config Release --target package   (-> ZIP in build/)
#
# This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

cmake_minimum_required(VERSION 3.21)
project(AlienFXLights VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

# --------------------------------------------------------------------------
# Shared protocol core (AlienFX API v2–v8 + AW920K) behind a HID transport.
# --------------------------------------------------------------------------
set(CORE_SOURCES
    src/core/AlienFX_SDK.cpp
    src/core/AFXController.cpp)
if(WIN32)
    list(APPEND CORE_SOURCES src/core/HidTransport_Win32.cpp)
elseif(APPLE)
    list(APPEND CORE_SOURCES src/core/HidTransport_IOKit.mm)
endif()

add_library(alienfx_core STATIC ${CORE_SOURCES})
target_include_directories(alienfx_core PUBLIC src/core)

if(WIN32)
    target_compile_definitions(alienfx_core PUBLIC
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(alienfx_core PUBLIC hid setupapi)
elseif(APPLE)
    target_link_libraries(alienfx_core PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

# --------------------------------------------------------------------------
# Platform app
# --------------------------------------------------------------------------
if(WIN32)
    add_executable(AlienFXLights WIN32
        src/windows/main.cpp
        src/windows/TrayApp.cpp
        src/windows/Settings.cpp
        src/windows/AlienFXLights.rc)
    target_sources(AlienFXLights PRIVATE src/windows/app.manifest)
    target_link_libraries(AlienFXLights PRIVATE alienfx_core
        user32 shell32 gdi32 comctl32 comdlg32 advapi32)
    install(TARGETS AlienFXLights RUNTIME DESTINATION .)

elseif(APPLE)
    # Generate AppIcon.icns from the PNG at build time (needs iconutil/sips).
    set(ICNS "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
    add_custom_command(OUTPUT "${ICNS}"
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make-icns.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}/assets/appicon-1024.png" "${ICNS}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/appicon-1024.png"
        COMMENT "Generating AppIcon.icns")

    add_executable(AlienFXLights MACOSX_BUNDLE
        src/macos/main.mm
        src/macos/AppDelegate.mm
        "${ICNS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/menuicon.png")
    target_link_libraries(AlienFXLights PRIVATE alienfx_core
        "-framework AppKit" "-framework IOKit" "-framework ServiceManagement")
    set_target_properties(AlienFXLights PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist")
    set_source_files_properties("${ICNS}" "${CMAKE_CURRENT_SOURCE_DIR}/assets/menuicon.png"
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    install(TARGETS AlienFXLights BUNDLE DESTINATION .)
endif()

# --------------------------------------------------------------------------
# Packaging: `cmake --build build --config Release --target package`
# --------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "AlienFXLights")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "AlienFXLights-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_GENERATOR "ZIP")
include(CPack)
