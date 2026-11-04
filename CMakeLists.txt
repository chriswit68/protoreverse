cmake_minimum_required(VERSION 3.20)

project(protoreverse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROTOREVERSE_STATIC
    "Build a fully static protoreverse executable (Linux/POSIX)" OFF)
option(PROTOREVERSE_STATIC_RUNTIME
    "Link libstdc++/libgcc statically but keep libc dynamic" OFF)

if (PROTOREVERSE_STATIC AND PROTOREVERSE_STATIC_RUNTIME)
    message(FATAL_ERROR
        "PROTOREVERSE_STATIC and PROTOREVERSE_STATIC_RUNTIME are mutually exclusive")
endif()

if ((PROTOREVERSE_STATIC OR PROTOREVERSE_STATIC_RUNTIME) AND MSVC)
    # Make all targets use the static MSVC runtime; mixing /MT and /MD fails.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (POLICY CMP0167)
    # Use BoostConfig.cmake instead of the removed FindBoost module.
    cmake_policy(SET CMP0167 NEW)
endif()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# Probe the toolchain before building. A missing static runtime otherwise only
# shows up as a linker error at the end of a long build.
set(_protoreverse_static_probe "
#include <cmath>
#include <iostream>
#include <string>
int main() {
    std::cout << std::sqrt(4.0) << std::string(\"ok\") << std::endl;
    return 0;
}
")

if (PROTOREVERSE_STATIC AND NOT MSVC AND NOT APPLE)
    # Re-evaluate on every configure so installing the packages is enough.
    unset(PROTOREVERSE_HAVE_FULLY_STATIC_LIBS CACHE)
    cmake_push_check_state()
    set(CMAKE_REQUIRED_LINK_OPTIONS "-static" "-pthread")
    check_cxx_source_compiles("${_protoreverse_static_probe}"
        PROTOREVERSE_HAVE_FULLY_STATIC_LIBS)
    cmake_pop_check_state()

    if (NOT PROTOREVERSE_HAVE_FULLY_STATIC_LIBS)
        message(FATAL_ERROR
            "PROTOREVERSE_STATIC=ON requires static C and C++ runtime "
            "libraries, but a test link with -static failed.\n"
            "Install them and re-run CMake:\n"
            "  Fedora/RHEL:   sudo dnf install libstdc++-static glibc-static\n"
            "  Debian/Ubuntu: sudo apt install g++ libc6-dev\n"
            "Details: ${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml "
            "(older CMake: CMakeError.log)\n"
            "Use -DPROTOREVERSE_STATIC=OFF for a dynamically linked build, or "
            "-DPROTOREVERSE_STATIC_RUNTIME=ON to static-link only "
            "libstdc++/libgcc.")
    endif()
endif()

if (PROTOREVERSE_STATIC_RUNTIME AND NOT MSVC AND
    CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    unset(PROTOREVERSE_HAVE_STATIC_CXX_RUNTIME CACHE)
    cmake_push_check_state()
    set(CMAKE_REQUIRED_LINK_OPTIONS "-static-libgcc" "-static-libstdc++" "-pthread")
    check_cxx_source_compiles("${_protoreverse_static_probe}"
        PROTOREVERSE_HAVE_STATIC_CXX_RUNTIME)
    cmake_pop_check_state()

    if (NOT PROTOREVERSE_HAVE_STATIC_CXX_RUNTIME)
        message(FATAL_ERROR
            "PROTOREVERSE_STATIC_RUNTIME=ON requires libstdc++.a, but a test "
            "link with -static-libstdc++ failed.\n"
            "Install it and re-run CMake:\n"
            "  Fedora/RHEL:   sudo dnf install libstdc++-static\n"
            "  Debian/Ubuntu: sudo apt install g++\n"
            "Details: ${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml "
            "(older CMake: CMakeError.log)\n"
            "Use -DPROTOREVERSE_STATIC_RUNTIME=OFF for a dynamically linked "
            "build, or -DPROTOREVERSE_STATIC=ON for a fully static build.")
    endif()
endif()

find_package(Boost 1.74 REQUIRED)
find_package(Threads REQUIRED)

add_library(protoreverse_core
    src/log.cpp
    src/cli.cpp
    src/util/hexdump.cpp
    src/wire/reader.cpp
    src/wire/writer.cpp
    src/wire/validator.cpp
    src/framing/framer.cpp
    src/framing/encoder.cpp
    src/framing/spec.cpp
    src/framing/learner.cpp
    src/greeting/greeting.cpp
    src/infer/schema.cpp
    src/infer/type_resolver.cpp
    src/emit/proto_writer.cpp
    src/analysis/stream_analyzer.cpp
    src/net/session.cpp
    src/net/server.cpp
)

target_include_directories(protoreverse_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(protoreverse_core PUBLIC Boost::headers Threads::Threads)
target_compile_definitions(protoreverse_core PUBLIC BOOST_ASIO_NO_DEPRECATED)

if (MSVC)
    target_compile_options(protoreverse_core PRIVATE /W4)
else()
    target_compile_options(protoreverse_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(protoreverse src/main.cpp)
target_link_libraries(protoreverse PRIVATE protoreverse_core)

if (PROTOREVERSE_STATIC)
    if (APPLE)
        message(FATAL_ERROR
            "PROTOREVERSE_STATIC is not supported on macOS. "
            "Use -DPROTOREVERSE_STATIC_RUNTIME=ON instead.")
    endif()
    if (MSVC)
        # Static CRT is selected via CMAKE_MSVC_RUNTIME_LIBRARY above.
        message(STATUS "protoreverse: using the static MSVC runtime")
    else()
        target_link_options(protoreverse PRIVATE -static)
        message(STATUS "protoreverse: linking a fully static executable (-static)")
    endif()
elseif (PROTOREVERSE_STATIC_RUNTIME)
    if (MSVC)
        # Static CRT is selected via CMAKE_MSVC_RUNTIME_LIBRARY above.
        message(STATUS "protoreverse: using the static MSVC runtime")
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(protoreverse PRIVATE -static-libgcc -static-libstdc++)
        message(STATUS
            "protoreverse: linking libgcc/libstdc++ statically (libc stays dynamic)")
    else()
        message(WARNING
            "PROTOREVERSE_STATIC_RUNTIME is not supported for this compiler; "
            "continuing with dynamic linking")
    endif()
endif()

option(PROTOREVERSE_BUILD_TESTS "Build protoreverse tests" ON)
if (PROTOREVERSE_BUILD_TESTS)
    enable_testing()
    add_executable(protoreverse_tests
        tests/test_main.cpp
    )
    target_link_libraries(protoreverse_tests PRIVATE protoreverse_core)
    add_test(NAME protoreverse_tests COMMAND protoreverse_tests)
endif()
