add_test(NAME repo_guard.unit
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_repo_guard.py)
add_test(NAME repo_guard.tracked
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/repo_guard.py --tracked)
