add_library(sa_recomp_ir STATIC ir_fixups.cpp)
target_include_directories(sa_recomp_ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(sa_recomp_ir SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(sa_recomp_ir PUBLIC ${MA_LLVM_DEFINITIONS})
target_link_libraries(sa_recomp_ir PUBLIC remill)
target_compile_options(sa_recomp_ir PRIVATE -Wno-unused-parameter -Wno-pedantic)

add_executable(sa-recomp
    main.cpp
    analysis.cpp
    guest_image.cpp
    lift.cpp
    compile.cpp
)
target_link_libraries(sa-recomp PRIVATE sa_recomp_ir remill ma_state_layout MacAndreas::loader MacAndreas::manifest MacAndreas::settings)
target_compile_options(sa-recomp PRIVATE -Wno-unused-parameter -Wno-pedantic)
target_compile_definitions(sa-recomp PRIVATE
    MA_GUEST_INTRINSICS_BC="${MA_GUEST_INTRINSICS_BC}")
add_dependencies(sa-recomp ma_guest_intrinsics)
