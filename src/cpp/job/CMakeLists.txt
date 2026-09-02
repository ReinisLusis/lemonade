# Standalone build for the job library and jobctl.
#
# Configured on its own, this pulls in nothing but nlohmann/json — no curl, no
# mbedtls, no libwebsockets, no zstd — so the cross-language conformance
# harness can build a jobctl in seconds instead of waiting for the whole
# Lemonade dependency set to be fetched and compiled.
#
#   cmake -S src/cpp/job -B build/jobctl -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/jobctl --config Release
#
# When the root project adds this directory instead, it reuses the
# nlohmann_json target that is already configured there.

cmake_minimum_required(VERSION 3.16)
project(lemon_job LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.9 QUIET)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    # A local single-header copy is honoured first so an offline machine, or a
    # conformance run that must not touch the network, can drop json.hpp in and
    # build. Same version the root CMakeLists pins.
    find_path(LEMON_JOB_JSON_INCLUDE_DIR "nlohmann/json.hpp"
        HINTS "${CMAKE_CURRENT_BINARY_DIR}/include"
              "${CMAKE_SOURCE_DIR}/build/jobctl/include")
    if(LEMON_JOB_JSON_INCLUDE_DIR)
        add_library(nlohmann_json INTERFACE)
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
        target_include_directories(nlohmann_json INTERFACE ${LEMON_JOB_JSON_INCLUDE_DIR})
        message(STATUS "Using local nlohmann/json header at ${LEMON_JOB_JSON_INCLUDE_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03  # v3.11.3
            GIT_SHALLOW TRUE
        )
        set(JSON_BuildTests OFF CACHE INTERNAL "")
        set(JSON_Install OFF CACHE INTERNAL "")
        FetchContent_MakeAvailable(json)
    endif()
endif()

add_library(lemon_job STATIC
    record.cpp
    store.cpp
)
target_include_directories(lemon_job PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(lemon_job PUBLIC nlohmann_json::nlohmann_json)

add_executable(jobctl jobctl.cpp)
target_link_libraries(jobctl PRIVATE lemon_job)

# Only when this directory is the top-level project: the root CMakeLists
# deliberately disables bare add_test() in favour of its add_cpp_ci_test()
# helper, and the same test is registered there for the full build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_job_record_test "${CMAKE_CURRENT_SOURCE_DIR}/../../../test/cpp/test_job_record.cpp")
    if(EXISTS "${_job_record_test}")
        enable_testing()
        add_executable(test_job_record "${_job_record_test}")
        target_link_libraries(test_job_record PRIVATE lemon_job)
        add_test(NAME JobRecordTest COMMAND test_job_record)
    endif()
endif()
