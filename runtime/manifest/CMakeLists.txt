add_library(ma_manifest STATIC src/manifest.cpp)
add_library(MacAndreas::manifest ALIAS ma_manifest)
target_include_directories(ma_manifest PUBLIC include)
