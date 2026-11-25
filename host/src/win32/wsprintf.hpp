#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ma::host::win32 {

std::string format_wsprintf(std::string_view format, const std::function<uint32_t(unsigned)>& arg,
                            const std::function<std::string(uint32_t)>& read_string = {});

}
