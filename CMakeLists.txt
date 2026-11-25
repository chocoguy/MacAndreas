cmake_minimum_required(VERSION 3.28)

set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture")

project(MacAndreas VERSION 0.0.1 LANGUAGES C CXX)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    message(FATAL_ERROR "MacAndreas builds only on Apple Silicon macOS")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MACANDREAS_GAME_DIR "${CMAKE_SOURCE_DIR}/gamefiles/Grand Theft Auto San Andreas"
    CACHE PATH "Local game directory used by tests that need the user's own gta_sa.exe")

set(MACANDREAS_LLVM_MAJOR 22 CACHE STRING "LLVM major version used by Remill and lifted code")
set(MACANDREAS_LLVM_PREFIX "/opt/homebrew/opt/llvm@${MACANDREAS_LLVM_MAJOR}"
    CACHE PATH "LLVM install matching the Remill build")
set(MACANDREAS_LLVM_CLANG "${MACANDREAS_LLVM_PREFIX}/bin/clang"
    CACHE FILEPATH "clang used to compile lifted IR")
set(MACANDREAS_REMILL_LIFT "${CMAKE_SOURCE_DIR}/.cache/remill/install/bin/remill-lift-${MACANDREAS_LLVM_MAJOR}"
    CACHE FILEPATH "remill-lift built by tools/recomp/setup-remill.sh")

add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

enable_testing()

add_subdirectory(runtime/loader)
add_subdirectory(runtime/manifest)
add_subdirectory(tools/pe-inspect)
add_subdirectory(tools/repo-guard)

find_package(LLVM CONFIG QUIET
    PATHS "${MACANDREAS_LLVM_PREFIX}/lib/cmake/llvm" NO_DEFAULT_PATH)
if(LLVM_FOUND AND NOT LLVM_VERSION_MAJOR EQUAL MACANDREAS_LLVM_MAJOR)
    message(FATAL_ERROR "LLVM at ${MACANDREAS_LLVM_PREFIX} is ${LLVM_PACKAGE_VERSION}, expected major ${MACANDREAS_LLVM_MAJOR}")
endif()
set(MACANDREAS_REMILL_PREFIX "${CMAKE_SOURCE_DIR}/.cache/remill"
    CACHE PATH "Remill checkout built by tools/recomp/setup-remill.sh")

if(LLVM_FOUND)
    message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} found; building recompiler tools")
    add_subdirectory(tools/recomp/state-layout)
    list(APPEND CMAKE_PREFIX_PATH
        "${MACANDREAS_REMILL_PREFIX}/install"
        "${MACANDREAS_REMILL_PREFIX}/dependencies/install"
        "${MACANDREAS_LLVM_PREFIX}")
    find_package(remill CONFIG QUIET)
    if(remill_FOUND)
        message(STATUS "Remill found; building sa-recomp")
        add_subdirectory(runtime/recomp)
        add_subdirectory(tools/recomp/sa-recomp)
    else()
        message(STATUS "Remill not found under ${MACANDREAS_REMILL_PREFIX}; run tools/recomp/setup-remill.sh")
    endif()
else()
    message(STATUS "LLVM ${MACANDREAS_LLVM_MAJOR} not found at ${MACANDREAS_LLVM_PREFIX}; recompiler tools skipped")
endif()
add_subdirectory(host)
add_subdirectory(tests)
