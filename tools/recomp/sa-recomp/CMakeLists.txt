add_executable(sa-recomp
    main.cpp
    analysis.cpp
    guest_image.cpp
    ir_fixups.cpp
    lift.cpp
    compile.cpp
)
target_link_libraries(sa-recomp PRIVATE remill ma_state_layout MacAndreas::loader MacAndreas::manifest)
target_compile_options(sa-recomp PRIVATE -Wno-unused-parameter -Wno-pedantic)
target_compile_definitions(sa-recomp PRIVATE
    MA_GUEST_INTRINSICS_BC="${MA_GUEST_INTRINSICS_BC}")
add_dependencies(sa-recomp ma_guest_intrinsics)
