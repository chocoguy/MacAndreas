set(MA_GUEST_INTRINSICS_BC "${CMAKE_CURRENT_BINARY_DIR}/guest_intrinsics.bc")

execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MA_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)

add_custom_command(
    OUTPUT "${MA_GUEST_INTRINSICS_BC}"
    COMMAND "${MACANDREAS_LLVM_CLANG}"
        -target arm64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}
        -isysroot "${MA_SDK_PATH}"
        -O2 -std=c17 -fno-math-errno -ffp-model=strict
        -Wno-unused-parameter
        -emit-llvm -c "${CMAKE_CURRENT_SOURCE_DIR}/guest_intrinsics.c"
        -o "${MA_GUEST_INTRINSICS_BC}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/guest_intrinsics.c"
    COMMENT "Compiling guest intrinsics to LLVM bitcode"
    VERBATIM)

add_custom_target(ma_guest_intrinsics ALL DEPENDS "${MA_GUEST_INTRINSICS_BC}")

set(MA_GUEST_INTRINSICS_BC "${MA_GUEST_INTRINSICS_BC}" PARENT_SCOPE)
