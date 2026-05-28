cmake_minimum_required(VERSION 3.16)
project(lob_engine CXX)

# ── C++20 required ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++20, not -std=gnu++20

# ── Detect toolchain ──────────────────────────────────────────────────────────
if(WIN32)
    message(STATUS "Target: Windows (MSYS2/MinGW-w64)")
else()
    message(STATUS "Target: Linux/macOS")
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3
        -Wall
        -Wextra
        -Wpedantic
    )
    # march=native only if not cross-compiling
    if(NOT CMAKE_CROSSCOMPILING)
        add_compile_options(-march=native)
    endif()
elseif(MSVC)
    add_compile_options(/O2 /W4 /EHsc)
endif()

# ── Include directories ───────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Shared engine sources ─────────────────────────────────────────────────────
set(ENGINE_SRC
    src/order_book.cpp
    src/matching_engine.cpp
)

# ── Threading ─────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Target: demo ─────────────────────────────────────────────────────────────
add_executable(lob_demo
    src/main.cpp
    ${ENGINE_SRC}
)
target_link_libraries(lob_demo PRIVATE Threads::Threads)
set_target_properties(lob_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Target: benchmark ────────────────────────────────────────────────────────
add_executable(lob_bench
    bench/benchmark.cpp
    ${ENGINE_SRC}
)
target_link_libraries(lob_bench PRIVATE Threads::Threads)
set_target_properties(lob_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── Target: unit tests (doctest) ─────────────────────────────────────────────
set(DOCTEST_HEADER "${CMAKE_SOURCE_DIR}/tests/doctest.h")
if(EXISTS "${DOCTEST_HEADER}")
    add_executable(lob_tests
        tests/test_engine.cpp
        ${ENGINE_SRC}
    )
    target_include_directories(lob_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(lob_tests PRIVATE Threads::Threads)
    set_target_properties(lob_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # Register with CTest so "ctest" command works
    enable_testing()
    add_test(
        NAME unit_tests
        COMMAND lob_tests --reporters=console --no-intro
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    message(STATUS "doctest found -> unit test target enabled")
else()
    message(STATUS "tests/doctest.h not found -> skipping tests target")
    message(STATUS "  Run: scripts/get_doctest.sh  (Linux/macOS)")
    message(STATUS "  Or:  scripts\\get_doctest.bat (Windows)")
endif()

# ── Print final summary ───────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  lob_demo   -> bin/lob_demo(.exe)")
message(STATUS "  lob_bench  -> bin/lob_bench(.exe)")
if(EXISTS "${DOCTEST_HEADER}")
    message(STATUS "  lob_tests  -> bin/lob_tests(.exe)")
endif()
message(STATUS "")
