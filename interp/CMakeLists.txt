include(FetchContent)

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_FORMATTER ON CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG a2278f1d254e492f6a6b39f6cb5d1f5d515659dc
    GIT_SUBMODULES_RECURSE ON
    SYSTEM)
FetchContent_MakeAvailable(zydis)

add_library(ma_interp STATIC src/interpreter.cpp)
target_include_directories(ma_interp PUBLIC include "${CMAKE_SOURCE_DIR}/host/src")
target_include_directories(ma_interp SYSTEM PUBLIC "${MA_REMILL_INCLUDE}")
target_link_libraries(ma_interp PUBLIC MacAndreas::loader PRIVATE Zydis)
target_compile_options(ma_interp PRIVATE -Wno-gnu-anonymous-struct -Wno-nested-anon-types -Wno-pedantic)
