#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ma::tools {

inline std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

inline std::string json_hex(uint32_t v) {
    return std::format("\"0x{:08x}\"", v);
}

class JsonObject {
public:
    JsonObject& raw(std::string_view key, std::string value) {
        fields_.push_back(json_string(key) + ":" + std::move(value));
        return *this;
    }
    JsonObject& str(std::string_view key, std::string_view value) { return raw(key, json_string(value)); }
    JsonObject& hex(std::string_view key, uint32_t value) { return raw(key, json_hex(value)); }
    JsonObject& num(std::string_view key, uint64_t value) { return raw(key, std::to_string(value)); }
    JsonObject& boolean(std::string_view key, bool value) { return raw(key, value ? "true" : "false"); }

    std::string build() const {
        std::string out = "{";
        for (size_t i = 0; i < fields_.size(); ++i) {
            out += (i ? "," : "") + fields_[i];
        }
        return out + "}";
    }

private:
    std::vector<std::string> fields_;
};

inline std::string json_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        out += (i ? "," : "") + items[i];
    }
    return out + "]";
}

}
