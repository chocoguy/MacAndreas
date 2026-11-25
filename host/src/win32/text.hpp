#pragma once

#include <cstdint>

namespace ma::host::win32 {

uint32_t multibyte_to_wide(uint32_t src, uint32_t src_len, uint32_t dst, uint32_t dst_len);
uint32_t wide_to_multibyte(uint32_t src, uint32_t src_len, uint32_t dst, uint32_t dst_len, uint32_t used_default);
uint32_t string_type_a(uint32_t info_type, uint32_t src, uint32_t len, uint32_t out);
uint32_t string_type_w(uint32_t info_type, uint32_t src, uint32_t len, uint32_t out);
uint32_t lcmap_a(uint32_t flags, uint32_t src, uint32_t len, uint32_t dst, uint32_t dst_len);
uint32_t lcmap_w(uint32_t flags, uint32_t src, uint32_t len, uint32_t dst, uint32_t dst_len);
uint32_t compare_a(uint32_t flags, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len);
uint32_t compare_w(uint32_t flags, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len);
uint32_t locale_info(uint32_t type, uint32_t out, uint32_t out_len, bool wide);

}
