add_executable(test_pe_synthetic loader/test_pe_synthetic.cpp)
target_include_directories(test_pe_synthetic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pe_synthetic PRIVATE MacAndreas::loader)
add_test(NAME loader.pe_synthetic COMMAND test_pe_synthetic)

add_executable(test_manifest manifest/test_manifest.cpp)
target_include_directories(test_manifest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_manifest PRIVATE MacAndreas::manifest)
add_test(NAME manifest.parse_and_check COMMAND test_manifest)

add_executable(test_game_exe loader/test_game_exe.cpp)
target_include_directories(test_game_exe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_game_exe PRIVATE MacAndreas::loader)
add_test(NAME game.exe_identity COMMAND test_game_exe "${MACANDREAS_GAME_DIR}")
set_tests_properties(game.exe_identity PROPERTIES SKIP_RETURN_CODE 77 LABELS game)

add_executable(lift_smoke_host recomp/lift_smoke_host.cpp)
target_include_directories(lift_smoke_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lift_smoke_host PRIVATE MacAndreas::loader)
set_target_properties(lift_smoke_host PROPERTIES ENABLE_EXPORTS ON)

if(TARGET state-layout-check)
    add_test(NAME game.lift_smoke
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/recomp/lift_smoke.py
            --remill-lift "${MACANDREAS_REMILL_LIFT}"
            --clang "${MACANDREAS_LLVM_CLANG}"
            --host $<TARGET_FILE:lift_smoke_host>
            --layout-check $<TARGET_FILE:state-layout-check>
            --game-dir "${MACANDREAS_GAME_DIR}"
            --work "${CMAKE_CURRENT_BINARY_DIR}/lift_smoke")
    set_tests_properties(game.lift_smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS "game;recomp")
endif()

add_test(NAME game.sa_verify_accepts COMMAND sa-verify "${MACANDREAS_GAME_DIR}/gta_sa.exe")
set_tests_properties(game.sa_verify_accepts PROPERTIES LABELS game)
if(NOT EXISTS "${MACANDREAS_GAME_DIR}/gta_sa.exe")
    set_tests_properties(game.sa_verify_accepts PROPERTIES DISABLED TRUE)
endif()

if(TARGET sa-host-core)
    add_executable(test_host_runtime host/test_host_runtime.cpp)
    target_include_directories(test_host_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_host_runtime PRIVATE sa-host-core)
    add_test(NAME host.runtime COMMAND test_host_runtime)

    add_executable(test_audio_exit audio/test_audio_exit.cpp)
    target_link_libraries(test_audio_exit PRIVATE sa-host-core)
    add_test(NAME host.audio_exit COMMAND test_audio_exit "${CMAKE_CURRENT_BINARY_DIR}/audio_exit.wav")

    add_executable(test_gfx gfx/test_gfx.cpp)
    target_include_directories(test_gfx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_gfx PRIVATE sa-host-core)
    add_test(NAME host.gfx COMMAND test_gfx)
endif()
