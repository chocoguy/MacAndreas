add_executable(manifest-check manifest_check_main.cpp)
target_link_libraries(manifest-check PRIVATE MacAndreas::manifest MacAndreas::settings)
