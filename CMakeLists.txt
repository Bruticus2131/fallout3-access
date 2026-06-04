cmake_minimum_required(VERSION 3.21)

# Fallout 3 GOTY accessibility plugin (FOSE plugin DLL).
# Build with Visual Studio 2022 Build Tools + Windows SDK.
#
# Example configure (PowerShell):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 `
#         -DFOSE_SDK_PATH="C:/dev/fose" `
#         -DGAME_PATH="C:/Games/Fallout 3 GOTY"
#   cmake --build build --config Release
#
# Notes:
#   * FOSE plugins are 32-bit (Fallout3.exe is x86). The -A Win32 is mandatory.
#   * Fallout 3 was built with MSVC; we target /MT to avoid CRT mismatch
#     issues when the game loads us.

project(Fallout3Access
    VERSION 0.1.0
    DESCRIPTION "Fallout 3 accessibility mod (screen reader integration)"
    LANGUAGES CXX
)

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    message(FATAL_ERROR
        "FOSE plugins are 32-bit. Re-run cmake with: -A Win32"
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- Paths -------------------------------------------------------------------

set(FOSE_SDK_PATH "${CMAKE_SOURCE_DIR}/third_party/fose"
    CACHE PATH "Path to FOSE source repo root (containing common/ and fose/ folders)")
set(GAME_PATH "" CACHE PATH "Path to Fallout 3 GOTY install (folder containing Fallout3.exe)")

# Real FOSE clone layout:
#   <root>/common/IPrefix.h
#   <root>/fose/fose/PluginAPI.h        <- main FOSE DLL project
#   <root>/fose/fose_common/SafeWrite.h
# So our include paths need <root> AND <root>/fose.

if(NOT EXISTS "${FOSE_SDK_PATH}/fose/fose/PluginAPI.h")
    message(WARNING
        "FOSE_SDK_PATH='${FOSE_SDK_PATH}' does not contain fose/fose/PluginAPI.h. "
        "Configure with -DFOSE_SDK_PATH=... pointing at your FOSE source clone root."
    )
endif()

# ---- Tolk (third_party/tolk) -------------------------------------------------

set(TOLK_DIR "${CMAKE_SOURCE_DIR}/third_party/tolk")
if(NOT EXISTS "${TOLK_DIR}/include/Tolk.h")
    message(STATUS "Tolk headers not found in ${TOLK_DIR}/include. "
                   "We load Tolk.dll dynamically at runtime, so the header is "
                   "only required to compile the wrapper. See third_party/tolk/README.txt.")
endif()

# ---- The plugin DLL ----------------------------------------------------------

set(F3A_SOURCES
    src/plugin_main.cpp
    src/tolk_bridge.cpp
    src/console.cpp
    src/config.cpp
    src/strings_pl.cpp
    src/logger.cpp
    src/hotkeys.cpp
    src/menu_dispatch.cpp
    src/polling_loop.cpp
    src/engine_hooks.cpp
    src/game_access.cpp
    src/modules/pipboy.cpp
    src/modules/dialog.cpp
    src/modules/barter.cpp
    src/modules/lockpick.cpp
    src/modules/vats.cpp
    src/modules/container.cpp
    src/modules/message.cpp
    src/modules/nav_assist.cpp
    src/modules/world_scan.cpp
    src/modules/modules_init.cpp
)

add_library(Fallout3Access SHARED ${F3A_SOURCES})

target_include_directories(Fallout3Access
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/tolk/include
        ${FOSE_SDK_PATH}        # enables  #include "common/IPrefix.h"
        ${FOSE_SDK_PATH}/fose   # enables  #include "fose/PluginAPI.h"
                                #         and "fose_common/SafeWrite.h"
)

target_compile_definitions(Fallout3Access PRIVATE
    # Pin FOSE-source #if FALLOUT_VERSION ladders to the 1.7 branch (Steam
    # GOTY 1.7.0.3). This unlocks address tables inside GameAPI.h /
    # GameInterface.h that we read via fose_runtime.h.
    FALLOUT_VERSION=0x01070030
    RUNTIME
)

target_compile_definitions(Fallout3Access
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
        F3A_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        F3A_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        F3A_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_options(Fallout3Access PRIVATE
    /W3 /Zi /EHsc
    # FOSE source predates strict C++ conformance; /permissive- breaks
    # two-phase name lookup in GameTypes.h::tList.
    /permissive
    # Force-include our prefix on every TU: it pulls in FOSE's fose_prefix.h
    # (UInt32 typedefs + FALLOUT_VERSION_* macros) plus <string>, which
    # Tile::GetQualifiedName references but FOSE headers don't include.
    /FI"${CMAKE_SOURCE_DIR}/include/f3a/f3a_prefix.h"
    # Silence noisy FOSE source warnings we can't fix.
    /wd4201 /wd4324 /wd4458 /wd4459
)

target_link_libraries(Fallout3Access PRIVATE
    user32 kernel32 shell32 advapi32 shlwapi version
)

set_target_properties(Fallout3Access PROPERTIES
    OUTPUT_NAME "fallout3_access"
    PREFIX ""
    LINK_FLAGS "/DEBUG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS"
)

# ---- Deploy target -----------------------------------------------------------

if(GAME_PATH AND EXISTS "${GAME_PATH}/Fallout3.exe")
    set(PLUGIN_DIR "${GAME_PATH}/Data/FOSE/Plugins")
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Fallout3Access>"
                "${PLUGIN_DIR}/fallout3_access.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/data/Fallout3Access.ini"
                "${PLUGIN_DIR}/Fallout3Access.ini"
        DEPENDS Fallout3Access
        COMMENT "Deploying fallout3_access.dll to ${PLUGIN_DIR}"
    )
else()
    message(STATUS "GAME_PATH not set or invalid; 'deploy' target disabled. "
                   "Set -DGAME_PATH=... to enable in-place install.")
endif()
