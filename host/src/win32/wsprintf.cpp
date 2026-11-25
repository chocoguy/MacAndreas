#include "wsprintf.hpp"

#include "../guest_memory.hpp"

namespace ma::host::win32 {

namespace {

std::string pad(std::string body, size_t width, bool left, char fill, bool negative) {
    if (body.size() >= width) {
        return body;
    }
    size_t count = width - body.size();
    if (left) {
        return body + std::string(count, ' ');
    }
    if (fill == '0' && negative) {
        return "-" + std::string(count, '0') + body.substr(1);
    }
    return std::string(count, fill) + body;
}

std::string digits(uint32_t value, unsigned base, bool upper) {
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    do {
        out.insert(out.begin(), set[value % base]);
        value /= base;
    } while (value != 0);
    return out;
}

}

std::string format_wsprintf(std::string_view format, const std::function<uint32_t(unsigned)>& arg,
                            const std::function<std::string(uint32_t)>& read_string) {
    std::string out;
    unsigned next = 0;
    auto string_at = [&](uint32_t addr) -> std::string {
        if (addr == 0) {
            return "(null)";
        }
        return read_string ? read_string(addr) : mem::read_cstr(addr);
    };
    for (size_t i = 0; i < format.size(); ++i) {
        char ch = format[i];
        if (ch != '%') {
            out += ch;
            continue;
        }
        if (++i >= format.size()) {
            break;
        }
        bool left = false;
        char fill = ' ';
        bool prefix = false;
        for (; i < format.size(); ++i) {
            if (format[i] == '-') {
                left = true;
            } else if (format[i] == '0') {
                fill = '0';
            } else if (format[i] == '#') {
                prefix = true;
            } else {
                break;
            }
        }
        size_t width = 0;
        while (i < format.size() && format[i] >= '0' && format[i] <= '9') {
            width = width * 10 + static_cast<size_t>(format[i++] - '0');
        }
        size_t precision = 0;
        bool has_precision = false;
        if (i < format.size() && format[i] == '.') {
            has_precision = true;
            ++i;
            while (i < format.size() && format[i] >= '0' && format[i] <= '9') {
                precision = precision * 10 + static_cast<size_t>(format[i++] - '0');
            }
        }
        while (i < format.size() && (format[i] == 'l' || format[i] == 'h')) {
            ++i;
        }
        if (i >= format.size()) {
            break;
        }
        char conv = format[i];
        std::string body;
        bool negative = false;
        switch (conv) {
        case '%':
            out += '%';
            continue;
        case 'd':
        case 'i': {
            auto v = static_cast<int32_t>(arg(next++));
            negative = v < 0;
            uint32_t magnitude = negative ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
            body = digits(magnitude, 10, false);
            if (has_precision && body.size() < precision) {
                body.insert(0, precision - body.size(), '0');
            }
            if (negative) {
                body.insert(body.begin(), '-');
            }
            break;
        }
        case 'u':
            body = digits(arg(next++), 10, false);
            if (has_precision && body.size() < precision) {
                body.insert(0, precision - body.size(), '0');
            }
            break;
        case 'x':
        case 'X':
            body = digits(arg(next++), 16, conv == 'X');
            if (has_precision && body.size() < precision) {
                body.insert(0, precision - body.size(), '0');
            }
            if (prefix) {
                body.insert(0, conv == 'X' ? "0X" : "0x");
            }
            break;
        case 'c':
            body = std::string(1, static_cast<char>(arg(next++)));
            break;
        case 's':
            body = string_at(arg(next++));
            if (has_precision && body.size() > precision) {
                body.resize(precision);
            }
            break;
        default:
            out += '%';
            out += conv;
            continue;
        }
        out += pad(std::move(body), width, left, fill, negative);
    }
    return out;
}

}
