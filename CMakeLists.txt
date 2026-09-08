cmake_minimum_required(VERSION 3.20)
project(ffxiv-latency-mitigator VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce static runtime on Windows MSVC for true zero-dependency standalone binaries
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# 1. Core Logic Library (Cross-platform)
# -----------------------------------------------------------------------------
add_library(mitigator_core STATIC
    src/core/rolling_rtt.cpp
    src/core/sequence_tracker.cpp
    src/core/cast_tracker.cpp
    src/core/animation_lock.cpp
    src/core/ipc_protocol.cpp
    src/payload/sigscan.cpp
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
    tests/test_main.cpp
    tests/test_rolling_rtt.cpp
    tests/test_sequence_tracker.cpp
    tests/test_cast_tracker.cpp
    tests/test_animation_lock.cpp
    tests/test_ipc_protocol.cpp
    tests/test_sigscan.cpp
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

    # 3b. In-Game Payload DLL
    add_library(mitigator_payload SHARED
        src/payload/dllmain.cpp
        src/payload/game_hooks.cpp
        src/payload/payload_ipc.cpp
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

    # 3c. Payload DLL Byte-Embedding Header Generation
    set(EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_payload_generated.hpp")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_custom_command(
        OUTPUT "${EMBEDDED_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_dll.py"
                "$<TARGET_FILE:mitigator_payload>"
                "${EMBEDDED_HEADER}"
        DEPENDS mitigator_payload "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_dll.py"
        COMMENT "Embedding mitigator_payload.dll into C++ header..."
        VERBATIM
    )

    add_custom_target(generate_embedded_payload DEPENDS "${EMBEDDED_HEADER}")

    # 3d. Standalone Console Loader
    add_executable(ffxiv-mitigator
        src/loader/main.cpp
        src/loader/process_finder.cpp
        src/loader/injector.cpp
        src/loader/loader_ipc.cpp
        src/loader/ui_renderer.cpp
        "${EMBEDDED_HEADER}"
    )

    add_dependencies(ffxiv-mitigator generate_embedded_payload)

    target_include_directories(ffxiv-mitigator PRIVATE
        include
        src
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )

    target_link_libraries(ffxiv-mitigator PRIVATE mitigator_core)
endif()
