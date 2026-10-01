cmake_minimum_required(VERSION 3.20)

#
# Version handling.
#
# include/astra/version.h is the only place where the version is written down.
# It is parsed before project() so that the CMake project version is derived
# from the public header and the two can never drift apart.
#
set(ASTRA_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/astra/version.h")

foreach(_astra_part IN ITEMS MAJOR MINOR PATCH)
    file(STRINGS "${ASTRA_VERSION_HEADER}" _astra_line
         REGEX "^#define[ \t]+ASTRA_DB_VERSION_${_astra_part}[ \t]+[0-9]+[ \t]*$")
    if(NOT _astra_line)
        message(FATAL_ERROR
            "Could not parse ASTRA_DB_VERSION_${_astra_part} from ${ASTRA_VERSION_HEADER}. "
            "The define must stay on a single line as '#define ASTRA_DB_VERSION_${_astra_part} <integer>'.")
    endif()
    string(REGEX MATCH "[0-9]+" _astra_value "${_astra_line}")
    set(ASTRA_VERSION_${_astra_part} "${_astra_value}")
endforeach()

project(astra-db
    VERSION ${ASTRA_VERSION_MAJOR}.${ASTRA_VERSION_MINOR}.${ASTRA_VERSION_PATCH}
    DESCRIPTION "AstraDB - a high-performance enterprise RDBMS written in C"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Single configuration generators have no default; use Release so that a plain
# `cmake -S . -B build` produces an optimised build.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build configuration" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(ASTRA_BUILD_TESTS "Build the AstraDB test suite" ON)
option(ASTRA_WERROR "Treat compiler warnings as errors" ON)

set(ASTRA_SANITIZE "none" CACHE STRING
    "Sanitizers to enable: none, address, undefined, address+undefined")
set_property(CACHE ASTRA_SANITIZE PROPERTY STRINGS
    none address undefined address+undefined)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)

# Probe once, before any target is created, whether this MSVC exposes C11
# <stdatomic.h> behind an opt-in switch. The result is consumed by
# astra_target_warnings().
if(MSVC)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "/std:c17 /experimental:c11atomics")
    check_c_source_compiles("
        #include <stdatomic.h>
        int main(void) { atomic_int value; atomic_store(&value, 0); return atomic_load(&value); }
    " ASTRA_MSAN_HAS_C11_ATOMICS)
    unset(CMAKE_REQUIRED_FLAGS)

    if(NOT ASTRA_MSAN_HAS_C11_ATOMICS)
        message(FATAL_ERROR
            "This MSVC does not provide C11 <stdatomic.h>, which AstraDB requires. "
            "Update the Visual Studio installation, or build with GCC or Clang.")
    endif()
endif()

# Library: the public AstraDB C API. Static until a stable ABI is promised.
add_library(astradb STATIC
    src/core/astra_core.c
    src/core/allocator.c
    src/core/config.c
    src/core/error.c
    src/core/log.c
    src/core/sync.c
    src/core/types.c
    src/storage/database_file.c
    src/storage/disk_manager.c
    src/storage/page_io.c
    src/storage/buffer/buffer_frame.c
    src/storage/buffer/replacer.c
    src/storage/buffer/buffer_pool.c
)
add_library(astra::core ALIAS astradb)

target_include_directories(astradb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Private headers are named "core/<header>.h" from within the library
        # so that a private include path can never shadow a public one.
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(astradb PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

astra_target_warnings(astradb)
astra_target_sanitize(astradb)

# Command line entry point.
add_executable(astra
    src/main.c
)
target_link_libraries(astra PRIVATE astradb)

set_target_properties(astra PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

astra_target_warnings(astra)
astra_target_sanitize(astra)

if(ASTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "${PROJECT_NAME} ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  tests: ${ASTRA_BUILD_TESTS}")
message(STATUS "  warnings as errors: ${ASTRA_WERROR}")
message(STATUS "  sanitizers: ${ASTRA_SANITIZE}")