cmake_minimum_required(VERSION 3.25)  # 3.25: FetchContent SYSTEM
project(lob_engine CXX)

# ── C++20 required ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++20, not -std=gnu++20

# Release unless told otherwise. Release gives -O3 -DNDEBUG; Debug gives -O0 -g
# with assert() checks on.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_NATIVE "Tune for this machine's CPU (-march=native)" ON)
option(LOB_BUILD_TESTS "Build the unit tests (fetches doctest)" ON)

# ── Toolchain ─────────────────────────────────────────────────────────────────
if(WIN32)
    message(STATUS "Target: Windows (MSYS2/MinGW-w64)")
else()
    message(STATUS "Target: Linux/macOS")
endif()

# MinGW: link the C++ runtime statically so the programs run without MSYS2's DLLs on PATH.
if(MINGW)
    add_link_options(-static)
endif()

# ── The engine, shared by every program ───────────────────────────────────────
find_package(Threads REQUIRED)

add_library(lob_core
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(lob_core PUBLIC include)
target_link_libraries(lob_core PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lob_core PUBLIC -Wall -Wextra -Wpedantic)
    if(LOB_NATIVE AND NOT CMAKE_CROSSCOMPILING)
        target_compile_options(lob_core PUBLIC -march=native)
    endif()
elseif(MSVC)
    target_compile_options(lob_core PUBLIC /W4 /EHsc)
endif()

# ── Target: demo ─────────────────────────────────────────────────────────────
add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob_core)

# ── Target: benchmark ────────────────────────────────────────────────────────
add_executable(lob_bench bench/benchmark.cpp)
target_link_libraries(lob_bench PRIVATE lob_core)

set_target_properties(lob_demo lob_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Target: unit tests (doctest, pinned) ─────────────────────────────────────
if(LOB_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        2d0a9359a60c51affe2a9bebb1be1dca47868151  # v2.5.3
        SYSTEM
    )
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)

    add_executable(lob_tests tests/test_engine.cpp)
    # doctest.h is included directly, as "doctest.h"
    target_include_directories(lob_tests SYSTEM PRIVATE "${doctest_SOURCE_DIR}/doctest")
    target_link_libraries(lob_tests PRIVATE lob_core)
    set_target_properties(lob_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    enable_testing()
    add_test(NAME unit_tests COMMAND lob_tests --no-intro)
endif()

# ── Print final summary ───────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "Targets (${CMAKE_BUILD_TYPE}):")
message(STATUS "  lob_demo   -> bin/lob_demo(.exe)")
message(STATUS "  lob_bench  -> bin/lob_bench(.exe)")
if(LOB_BUILD_TESTS)
    message(STATUS "  lob_tests  -> bin/lob_tests(.exe)")
endif()
message(STATUS "")
