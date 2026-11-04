cmake_minimum_required(VERSION 3.20)

project(protoreverse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (POLICY CMP0167)
    # Use BoostConfig.cmake instead of the removed FindBoost module.
    cmake_policy(SET CMP0167 NEW)
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

option(PROTOREVERSE_BUILD_TESTS "Build protoreverse tests" ON)
if (PROTOREVERSE_BUILD_TESTS)
    enable_testing()
    add_executable(protoreverse_tests
        tests/test_main.cpp
    )
    target_link_libraries(protoreverse_tests PRIVATE protoreverse_core)
    add_test(NAME protoreverse_tests COMMAND protoreverse_tests)
endif()
