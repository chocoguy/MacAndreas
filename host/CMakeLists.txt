set(MA_REMILL_INCLUDE "${MACANDREAS_REMILL_PREFIX}/install/include")
if(NOT EXISTS "${MA_REMILL_INCLUDE}/remill/Arch/X86/Runtime/State.h")
    message(STATUS "sa-host skipped: Remill headers not found under ${MA_REMILL_INCLUDE}")
    return()
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/interp" "${CMAKE_BINARY_DIR}/interp")

enable_language(OBJCXX)

add_library(sa-host-core OBJECT
    src/guest_memory.cpp
    src/thread.cpp
    src/thunks.cpp
    src/dispatch.cpp
    src/crash.cpp
    src/win32/process.cpp
    src/win32/kernel32.cpp
    src/win32/text.cpp
    src/win32/small_modules.cpp
    src/win32/user32.cpp
    src/win32/keymap.cpp
    src/win32/wsprintf.cpp
    src/win32/sync.cpp
    src/win32/seh.cpp
    src/win32/paths.cpp
    src/win32/files.cpp
    src/win32/com.cpp
    src/win32/ddraw.cpp
    src/win32/dsound.cpp
    src/win32/dinput8.cpp
    src/win32/dshow.cpp
    src/input/input_state.cpp
    src/input/keys.cpp
    src/native/natives.cpp
    src/manifest_runtime.cpp
    src/settings.cpp
    src/gfx/ff.cpp
    src/gfx/formats.cpp
    src/gfx/d3d9.cpp
    src/gfx/renderer_metal.mm
    src/platform/window.mm
    src/media/movie.cpp
    src/media/pl_mpeg_impl.c
)
set_source_files_properties(src/media/pl_mpeg_impl.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/gfx/renderer_metal.mm src/platform/window.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(sa-host-core PUBLIC src)
target_include_directories(sa-host-core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/pl_mpeg")
target_include_directories(sa-host-core SYSTEM PUBLIC "${MA_REMILL_INCLUDE}")
target_link_libraries(sa-host-core PUBLIC MacAndreas::loader MacAndreas::manifest ma_interp
    "-framework CoreGraphics" "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework ImageIO")
target_compile_options(sa-host-core PUBLIC -Wno-unused-parameter -Wno-pedantic -Wno-gnu-anonymous-struct
    -Wno-nested-anon-types)

add_executable(sa-host src/main.cpp)
target_link_libraries(sa-host PRIVATE sa-host-core)
set_target_properties(sa-host PROPERTIES ENABLE_EXPORTS ON)
