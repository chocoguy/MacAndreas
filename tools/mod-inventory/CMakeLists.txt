add_test(NAME mod_inventory.unit
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_mod_inventory.py)
add_test(NAME mod_inventory.check
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/mod_inventory.py check --require
        --manifest ${CMAKE_SOURCE_DIR}/manifest)
