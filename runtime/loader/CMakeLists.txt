add_library(ma_loader STATIC
    src/pe_image.cpp
    src/exe_identity.cpp
)
add_library(MacAndreas::loader ALIAS ma_loader)

target_include_directories(ma_loader PUBLIC include)
