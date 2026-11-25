#pragma once

#include <cstdint>

namespace ma::host::win32 {

bool is_filter_graph(uint32_t clsid);
uint32_t create_filter_graph(uint32_t riid, uint32_t out);
void tick_movies();

}
