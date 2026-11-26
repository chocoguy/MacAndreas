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
    src/difftest.cpp
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
    src/win32/vorbisfile.cpp
    src/audio/mix.cpp
    src/audio/stream.cpp
    src/audio/output.mm
    src/input/gamepad.mm
    src/input/input_state.cpp
    src/input/keys.cpp
    src/native/natives.cpp
    src/manifest_runtime.cpp
    src/settings.cpp
    src/gfx/ff.cpp
    src/gfx/sm.cpp
    src/gfx/sm_msl.cpp
    src/gfx/formats.cpp
    src/gfx/d3d9.cpp
    src/gfx/renderer_metal.mm
    src/platform/window.mm
    src/media/movie.cpp
    src/media/pl_mpeg_impl.c
)
set_source_files_properties(src/media/pl_mpeg_impl.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/gfx/renderer_metal.mm src/platform/window.mm src/audio/output.mm src/input/gamepad.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(sa-host-core PUBLIC src)
target_include_directories(sa-host-core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/pl_mpeg")
find_path(MA_VORBIS_INCLUDE vorbis/vorbisfile.h HINTS /opt/homebrew/opt/libvorbis/include REQUIRED)
find_path(MA_OGG_INCLUDE ogg/ogg.h HINTS /opt/homebrew/opt/libogg/include REQUIRED)
find_library(MA_VORBISFILE_LIB NAMES libvorbisfile.a HINTS /opt/homebrew/opt/libvorbis/lib REQUIRED)
find_library(MA_VORBIS_LIB NAMES libvorbis.a HINTS /opt/homebrew/opt/libvorbis/lib REQUIRED)
find_library(MA_OGG_LIB NAMES libogg.a HINTS /opt/homebrew/opt/libogg/lib REQUIRED)
target_include_directories(sa-host-core SYSTEM PRIVATE "${MA_VORBIS_INCLUDE}" "${MA_OGG_INCLUDE}")
target_link_libraries(sa-host-core PRIVATE "${MA_VORBISFILE_LIB}" "${MA_VORBIS_LIB}" "${MA_OGG_LIB}")
target_include_directories(sa-host-core SYSTEM PUBLIC "${MA_REMILL_INCLUDE}")
target_link_libraries(sa-host-core PUBLIC MacAndreas::loader MacAndreas::manifest ma_interp
    "-framework CoreGraphics" "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework ImageIO"
    "-framework AudioToolbox" "-framework CoreAudio" "-framework GameController")
target_compile_options(sa-host-core PUBLIC -Wno-unused-parameter -Wno-pedantic -Wno-gnu-anonymous-struct
    -Wno-nested-anon-types)

add_executable(sa-host src/main.cpp)
target_link_libraries(sa-host PRIVATE sa-host-core)
set_target_properties(sa-host PROPERTIES ENABLE_EXPORTS ON)
