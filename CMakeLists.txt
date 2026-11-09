cmake_minimum_required(VERSION 3.20)
project(ouch_order_entry VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(OUCH_BUILD_TESTS "Build unit and integration tests" ON)
option(OUCH_SANITIZE "Build tests with ASan + UBSan" ON)
option(OUCH_WERROR "Treat warnings as errors" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(OUCH_WERROR)
    add_compile_options(-Werror)
endif()

find_package(Threads REQUIRED)

set(OUCH_SOURCES
    src/messages.cpp
    src/soupbin.cpp
    src/exchange.cpp
    src/net.cpp
    src/sim_server.cpp
    src/client.cpp
)

add_library(ouch STATIC ${OUCH_SOURCES})
target_include_directories(ouch PUBLIC include)
target_link_libraries(ouch PUBLIC Threads::Threads)

# ── Tools ─────────────────────────────────────────────────────────────────────
add_executable(ouch_sim tools/ouch_sim.cpp)
target_link_libraries(ouch_sim PRIVATE ouch)

add_executable(ouch_client tools/ouch_client.cpp)
target_link_libraries(ouch_client PRIVATE ouch)

add_executable(ouch_latency bench/ouch_latency.cpp)
target_link_libraries(ouch_latency PRIVATE ouch)

add_executable(ouch_codec_bench bench/codec_bench.cpp)
target_link_libraries(ouch_codec_bench PRIVATE ouch)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(OUCH_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_library(ouch_test_lib STATIC ${OUCH_SOURCES})
    target_include_directories(ouch_test_lib PUBLIC include)
    target_link_libraries(ouch_test_lib PUBLIC Threads::Threads)

    add_executable(ouch_tests
        tests/test_codec.cpp
        tests/test_soupbin.cpp
        tests/test_exchange.cpp
        tests/test_integration.cpp
    )
    target_link_libraries(ouch_tests PRIVATE ouch_test_lib GTest::gtest_main)

    if(OUCH_SANITIZE)
        foreach(t ouch_test_lib ouch_tests)
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${t} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()

    include(GoogleTest)
    gtest_discover_tests(ouch_tests DISCOVERY_MODE PRE_TEST)
endif()
