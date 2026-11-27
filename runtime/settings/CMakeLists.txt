add_library(ma_settings STATIC src/settings.cpp)
add_library(MacAndreas::settings ALIAS ma_settings)
target_include_directories(ma_settings PUBLIC include)
