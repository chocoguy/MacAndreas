add_executable(pe-inspect pe_inspect_main.cpp)
target_link_libraries(pe-inspect PRIVATE MacAndreas::loader)

add_executable(sa-verify sa_verify_main.cpp)
target_link_libraries(sa-verify PRIVATE MacAndreas::loader)
