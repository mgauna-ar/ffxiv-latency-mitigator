cmake_minimum_required(VERSION 3.20)
project(ffxiv-latency-mitigator VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce static runtime (/MT) on Windows MSVC for true zero-dependency standalone binaries
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/W3>")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Unified binary output directories across multi-config and single-config generators
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

# -----------------------------------------------------------------------------
# 1. Core Logic Library (Cross-platform)
# -----------------------------------------------------------------------------
add_library(mitigator_core STATIC
    src/core/rolling_rtt.cpp
    src/core/sequence_tracker.cpp
    src/core/cast_tracker.cpp
    src/core/animation_lock.cpp
    src/core/ipc_protocol.cpp
    src/core/sigscan.cpp
    src/core/config_manager.cpp
)

target_include_directories(mitigator_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# -----------------------------------------------------------------------------
# 2. Cross-Platform Unit Tests
# -----------------------------------------------------------------------------
enable_testing()

add_executable(mitigator_tests
    src/loader/ui_renderer.cpp
    tests/test_main.cpp
    tests/test_rolling_rtt.cpp
    tests/test_sequence_tracker.cpp
    tests/test_cast_tracker.cpp
    tests/test_animation_lock.cpp
    tests/test_ipc_protocol.cpp
    tests/test_sigscan.cpp
    tests/test_ui_renderer.cpp
    tests/test_config_manager.cpp
)

target_include_directories(mitigator_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_link_libraries(mitigator_tests PRIVATE mitigator_core)

add_test(NAME unit_tests COMMAND mitigator_tests)

# -----------------------------------------------------------------------------
# 3. Windows Native Targets: Payload DLL & Standalone Loader Executable
# -----------------------------------------------------------------------------
if(WIN32 OR CYGWIN OR MSYS OR MINGW)
    # 3a. MinHook Embedded Library
    set(MINHOOK_SOURCES
        src/third_party/minhook/src/buffer.c
        src/third_party/minhook/src/hook.c
        src/third_party/minhook/src/trampoline.c
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND MINHOOK_SOURCES src/third_party/minhook/src/hde/hde64.c)
    else()
        list(APPEND MINHOOK_SOURCES src/third_party/minhook/src/hde/hde32.c)
    endif()

    add_library(minhook STATIC ${MINHOOK_SOURCES})
    target_include_directories(minhook PUBLIC src/third_party/minhook/include)
    if(MSVC)
        target_compile_options(minhook PRIVATE /W3 /wd4201 /wd4244 /wd4310)
        set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # 3b. In-Game Payload DLL
    add_library(mitigator_payload SHARED
        src/payload/dllmain.cpp
        src/payload/game_hooks.cpp
        src/payload/payload_ipc.cpp
        src/payload/pe_scanner.cpp
    )

    target_include_directories(mitigator_payload PRIVATE
        include
        src
        src/third_party/minhook/include
    )

    target_link_libraries(mitigator_payload PRIVATE
        mitigator_core
        minhook
    )

    # 3c. Standalone Console Loader
    add_executable(ffxiv-mitigator
        src/loader/main.cpp
        src/loader/process_finder.cpp
        src/loader/injector.cpp
        src/loader/loader_ipc.cpp
        src/loader/ui_renderer.cpp
    )

    target_include_directories(ffxiv-mitigator PRIVATE
        include
        src
    )

    target_link_libraries(ffxiv-mitigator PRIVATE
        mitigator_core
        advapi32
    )
endif()

