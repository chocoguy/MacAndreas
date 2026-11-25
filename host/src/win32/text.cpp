#include "text.hpp"

#include "process.hpp"

#include "../guest_memory.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kErrorInsufficientBuffer = 122;

constexpr uint16_t C1_UPPER = 0x0001;
constexpr uint16_t C1_LOWER = 0x0002;
constexpr uint16_t C1_DIGIT = 0x0004;
constexpr uint16_t C1_SPACE = 0x0008;
constexpr uint16_t C1_PUNCT = 0x0010;
constexpr uint16_t C1_CNTRL = 0x0020;
constexpr uint16_t C1_BLANK = 0x0040;
constexpr uint16_t C1_XDIGIT = 0x0080;
constexpr uint16_t C1_ALPHA = 0x0100;

constexpr uint32_t LCMAP_LOWERCASE = 0x100;
constexpr uint32_t LCMAP_UPPERCASE = 0x200;
constexpr uint32_t NORM_IGNORECASE = 0x1;

uint16_t ctype1(uint32_t ch) {
    if (ch < 0x20 || ch == 0x7f) {
        uint16_t t = C1_CNTRL;
        if (ch >= 0x09 && ch <= 0x0d) {
            t |= C1_SPACE;
        }
        if (ch == 0x09) {
            t |= C1_BLANK;
        }
        return t;
    }
    if (ch == 0x20) {
        return C1_SPACE | C1_BLANK;
    }
    if (ch >= '0' && ch <= '9') {
        return C1_DIGIT | C1_XDIGIT;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return C1_UPPER | C1_ALPHA | (ch <= 'F' ? C1_XDIGIT : 0);
    }
    if (ch >= 'a' && ch <= 'z') {
        return C1_LOWER | C1_ALPHA | (ch <= 'f' ? C1_XDIGIT : 0);
    }
    if (ch < 0x80) {
        return C1_PUNCT;
    }
    if (ch >= 0xc0 && ch <= 0xde && ch != 0xd7) {
        return C1_UPPER | C1_ALPHA;
    }
    if (ch >= 0xdf && ch <= 0xff && ch != 0xf7) {
        return C1_LOWER | C1_ALPHA;
    }
    if (ch == 0xa0) {
        return C1_SPACE | C1_BLANK;
    }
    return C1_PUNCT;
}

uint32_t to_lower(uint32_t ch) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xc0 && ch <= 0xde && ch != 0xd7)) {
        return ch + 0x20;
    }
    return ch;
}

uint32_t to_upper(uint32_t ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0xe0 && ch <= 0xfe && ch != 0xf7)) {
        return ch - 0x20;
    }
    return ch;
}

std::vector<uint32_t> read_units(uint32_t src, uint32_t len, bool wide) {
    std::vector<uint32_t> out;
    if (static_cast<int32_t>(len) < 0) {
        for (uint32_t i = 0;; ++i) {
            uint32_t ch = wide ? mem::read<uint16_t>(src + i * 2) : mem::read<uint8_t>(src + i);
            out.push_back(ch);
            if (ch == 0) {
                break;
            }
        }
        return out;
    }
    for (uint32_t i = 0; i < len; ++i) {
        out.push_back(wide ? mem::read<uint16_t>(src + i * 2) : mem::read<uint8_t>(src + i));
    }
    return out;
}

uint32_t write_units(const std::vector<uint32_t>& units, uint32_t dst, uint32_t dst_len, bool wide) {
    uint32_t n = static_cast<uint32_t>(units.size());
    if (dst_len == 0) {
        return n;
    }
    if (dst_len < n) {
        set_last_error(kErrorInsufficientBuffer);
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (wide) {
            mem::write<uint16_t>(dst + i * 2, static_cast<uint16_t>(units[i]));
        } else {
            mem::write<uint8_t>(dst + i, static_cast<uint8_t>(units[i] > 0xff ? '?' : units[i]));
        }
    }
    return n;
}

uint32_t string_type(uint32_t info_type, uint32_t src, uint32_t len, uint32_t out, bool wide) {
    auto units = read_units(src, len, wide);
    for (size_t i = 0; i < units.size(); ++i) {
        mem::write<uint16_t>(out + static_cast<uint32_t>(i * 2), info_type == 1 ? ctype1(units[i]) : 0);
    }
    return 1;
}

uint32_t lcmap(uint32_t flags, uint32_t src, uint32_t len, uint32_t dst, uint32_t dst_len, bool wide) {
    auto units = read_units(src, len, wide);
    for (auto& u : units) {
        if (flags & LCMAP_LOWERCASE) {
            u = to_lower(u);
        } else if (flags & LCMAP_UPPERCASE) {
            u = to_upper(u);
        }
    }
    return write_units(units, dst, dst_len, wide);
}

uint32_t compare(uint32_t flags, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len, bool wide) {
    auto x = read_units(a, a_len, wide);
    auto y = read_units(b, b_len, wide);
    if (!x.empty() && x.back() == 0 && static_cast<int32_t>(a_len) < 0) {
        x.pop_back();
    }
    if (!y.empty() && y.back() == 0 && static_cast<int32_t>(b_len) < 0) {
        y.pop_back();
    }
    size_t n = std::min(x.size(), y.size());
    for (size_t i = 0; i < n; ++i) {
        uint32_t p = flags & NORM_IGNORECASE ? to_lower(x[i]) : x[i];
        uint32_t q = flags & NORM_IGNORECASE ? to_lower(y[i]) : y[i];
        if (p != q) {
            return p < q ? 1 : 3;
        }
    }
    return x.size() == y.size() ? 2 : (x.size() < y.size() ? 1 : 3);
}

const std::unordered_map<uint32_t, std::string_view>& locale_table() {
    static const std::unordered_map<uint32_t, std::string_view> table = {
        {0x0001, "0409"},
        {0x0002, "English (United States)"},
        {0x0003, "ENU"},
        {0x0005, "1"},
        {0x0006, "United States"},
        {0x0007, "USA"},
        {0x0009, "0409"},
        {0x000a, "1"},
        {0x000b, "437"},
        {0x000e, "."},
        {0x000f, ","},
        {0x0010, "3;0"},
        {0x0014, "$"},
        {0x0015, "USD"},
        {0x0016, "."},
        {0x0017, ","},
        {0x0018, "3;0"},
        {0x0019, "2"},
        {0x001a, "2"},
        {0x001b, "0"},
        {0x001c, "0"},
        {0x001d, "/"},
        {0x001e, ":"},
        {0x001f, "M/d/yyyy"},
        {0x0020, "dddd, MMMM dd, yyyy"},
        {0x0021, "0"},
        {0x0022, "0"},
        {0x0023, "0"},
        {0x0028, "AM"},
        {0x0029, "PM"},
        {0x002a, "Monday"},
        {0x002b, "Tuesday"},
        {0x002c, "Wednesday"},
        {0x002d, "Thursday"},
        {0x002e, "Friday"},
        {0x002f, "Saturday"},
        {0x0030, "Sunday"},
        {0x0031, "Mon"},
        {0x0032, "Tue"},
        {0x0033, "Wed"},
        {0x0034, "Thu"},
        {0x0035, "Fri"},
        {0x0036, "Sat"},
        {0x0037, "Sun"},
        {0x0038, "January"},
        {0x0039, "February"},
        {0x003a, "March"},
        {0x003b, "April"},
        {0x003c, "May"},
        {0x003d, "June"},
        {0x003e, "July"},
        {0x003f, "August"},
        {0x0040, "September"},
        {0x0041, "October"},
        {0x0042, "November"},
        {0x0043, "December"},
        {0x0044, "Jan"},
        {0x0045, "Feb"},
        {0x0046, "Mar"},
        {0x0047, "Apr"},
        {0x0048, "May"},
        {0x0049, "Jun"},
        {0x004a, "Jul"},
        {0x004b, "Aug"},
        {0x004c, "Sep"},
        {0x004d, "Oct"},
        {0x004e, "Nov"},
        {0x004f, "Dec"},
        {0x0050, ""},
        {0x0051, "-"},
        {0x0052, "3"},
        {0x0053, "0"},
        {0x0054, "1"},
        {0x0055, "0"},
        {0x0056, "1"},
        {0x0057, "0"},
        {0x1001, "English"},
        {0x1002, "United States"},
        {0x1004, "1252"},
    };
    return table;
}

}

uint32_t multibyte_to_wide(uint32_t src, uint32_t src_len, uint32_t dst, uint32_t dst_len) {
    return write_units(read_units(src, src_len, false), dst, dst_len, true);
}

uint32_t wide_to_multibyte(uint32_t src, uint32_t src_len, uint32_t dst, uint32_t dst_len, uint32_t used_default) {
    auto units = read_units(src, src_len, true);
    bool replaced = false;
    for (auto u : units) {
        replaced |= u > 0xff;
    }
    if (used_default != 0) {
        mem::write<uint32_t>(used_default, replaced ? 1 : 0);
    }
    return write_units(units, dst, dst_len, false);
}

uint32_t string_type_a(uint32_t info_type, uint32_t src, uint32_t len, uint32_t out) {
    return string_type(info_type, src, len, out, false);
}

uint32_t string_type_w(uint32_t info_type, uint32_t src, uint32_t len, uint32_t out) {
    return string_type(info_type, src, len, out, true);
}

uint32_t lcmap_a(uint32_t flags, uint32_t src, uint32_t len, uint32_t dst, uint32_t dst_len) {
    return lcmap(flags, src, len, dst, dst_len, false);
}

uint32_t lcmap_w(uint32_t flags, uint32_t src, uint32_t len, uint32_t dst, uint32_t dst_len) {
    return lcmap(flags, src, len, dst, dst_len, true);
}

uint32_t compare_a(uint32_t flags, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
    return compare(flags, a, a_len, b, b_len, false);
}

uint32_t compare_w(uint32_t flags, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
    return compare(flags, a, a_len, b, b_len, true);
}

uint32_t locale_info(uint32_t type, uint32_t out, uint32_t out_len, bool wide) {
    bool as_number = (type & 0x20000000) != 0;
    uint32_t base = type & 0x0fffffff;
    const auto& table = locale_table();
    auto it = table.find(base);
    if (it == table.end()) {
        std::fprintf(stderr, "[LOCALE] unknown locale info type 0x%x\n", type);
        return 0;
    }
    if (as_number) {
        if (out_len >= (wide ? 2u : 4u)) {
            mem::write<uint32_t>(out, static_cast<uint32_t>(std::stoul(std::string(it->second))));
        }
        return wide ? 2 : 4;
    }
    std::vector<uint32_t> units(it->second.begin(), it->second.end());
    units.push_back(0);
    return write_units(units, out, out_len, wide);
}

}
