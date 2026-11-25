add_executable(test_pe_synthetic loader/test_pe_synthetic.cpp)
target_include_directories(test_pe_synthetic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pe_synthetic PRIVATE MacAndreas::loader)
add_test(NAME loader.pe_synthetic COMMAND test_pe_synthetic)

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
