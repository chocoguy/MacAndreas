#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ma::host::calls {

struct ScriptedCall {
    uint64_t frame;
    uint32_t target;
    std::string conv;
    std::vector<std::string> args;
    std::string text;
};

std::optional<ScriptedCall> parse_call(std::string_view text);
void add_call(ScriptedCall call);
void apply_calls(uint64_t frame);

}
