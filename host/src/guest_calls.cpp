#include "guest_calls.hpp"

#include "abi.hpp"
#include "guest_memory.hpp"
#include "thread.hpp"

#include <bit>
#include <charconv>
#include <cstdio>
#include <mutex>

namespace ma::host::calls {

namespace {

std::vector<ScriptedCall>& pending() {
    static std::vector<ScriptedCall> calls;
    return calls;
}

uint32_t g_last_result = 0;

std::optional<uint32_t> parse_word(std::string_view text, bool deref = false) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '$') {
        if (!deref) {
            return 0u;
        }
        auto body = text.substr(1);
        uint32_t addr = allocator().alloc(static_cast<uint32_t>(body.size() + 1));
        mem::write_cstr(addr, body);
        return addr;
    }
    if (text.front() == '@') {
        uint32_t offset = 0;
        if (text.size() > 1) {
            if (text[1] != '+') {
                return std::nullopt;
            }
            auto value = parse_word(text.substr(2));
            if (!value) {
                return std::nullopt;
            }
            offset = *value;
        }
        return deref ? g_last_result + offset : offset;
    }
    if (text.front() == '*') {
        auto addr = parse_word(text.substr(1));
        if (!addr) {
            return std::nullopt;
        }
        return deref ? mem::read<uint32_t>(*addr) : *addr;
    }
    if (text.back() == 'f') {
        float f = 0;
        auto body = text.substr(0, text.size() - 1);
        auto [end, ec] = std::from_chars(body.data(), body.data() + body.size(), f);
        if (ec != std::errc{} || end != body.data() + body.size()) {
            return std::nullopt;
        }
        return std::bit_cast<uint32_t>(f);
    }
    bool negative = text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    int base = 10;
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
        base = 16;
    }
    uint32_t v = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v, base);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return negative ? 0u - v : v;
}

}

std::optional<ScriptedCall> parse_call(std::string_view text) {
    ScriptedCall call{};
    call.text = std::string(text);
    std::vector<std::string_view> parts;
    while (true) {
        auto colon = text.find(':');
        parts.push_back(text.substr(0, colon));
        if (colon == std::string_view::npos) {
            break;
        }
        text = text.substr(colon + 1);
    }
    if (parts.size() < 3 || parts.size() > 4) {
        return std::nullopt;
    }
    auto [end, ec] = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), call.frame);
    auto target = parse_word(parts[1]);
    if (ec != std::errc{} || end != parts[0].data() + parts[0].size() || !target) {
        return std::nullopt;
    }
    call.target = *target;
    call.conv = std::string(parts[2]);
    if (call.conv != "cdecl" && call.conv != "stdcall" && call.conv != "thiscall") {
        return std::nullopt;
    }
    if (parts.size() == 4 && !parts[3].empty()) {
        std::string_view list = parts[3];
        while (true) {
            auto comma = list.find(',');
            auto item = list.substr(0, comma);
            if (!parse_word(item)) {
                return std::nullopt;
            }
            call.args.emplace_back(item);
            if (comma == std::string_view::npos) {
                break;
            }
            list = list.substr(comma + 1);
        }
    }
    if (call.conv == "thiscall" && call.args.empty()) {
        return std::nullopt;
    }
    return call;
}

void add_call(ScriptedCall call) { pending().push_back(std::move(call)); }

void apply_calls(uint64_t frame) {
    for (const auto& call : pending()) {
        if (call.frame != frame) {
            continue;
        }
        GuestThread* thread = current_thread();
        if (!thread) {
            std::fprintf(stderr, "[CALL] frame %llu: no guest thread for %s\n", static_cast<unsigned long long>(frame),
                         call.text.c_str());
            continue;
        }
        GuestState& s = thread->state;
        uint32_t saved_eax = eax(s);
        uint32_t saved_ecx = ecx(s);
        uint32_t saved_edx = edx(s);
        std::vector<uint32_t> words;
        for (const auto& a : call.args) {
            words.push_back(*parse_word(a, true));
        }
        uint32_t this_ptr = 0;
        std::span<const uint32_t> stack(words);
        if (call.conv == "thiscall") {
            this_ptr = words.front();
            ecx(s) = this_ptr;
            stack = stack.subspan(1);
        }
        uint16_t top_before = s.x87.fxsave32.swd.top;
        call_guest(s, call.target, stack, call.conv != "cdecl");
        uint32_t result = eax(s);
        g_last_result = result;
        bool pushed = s.x87.fxsave32.swd.top != top_before;
        double st0 = pushed ? x87_pop(s) : 0.0;
        if (pushed) {
            std::fprintf(stderr, "[CALL] frame %llu: %s = 0x%08x (st0 %g)\n", static_cast<unsigned long long>(frame),
                         call.text.c_str(), result, st0);
        } else {
            std::fprintf(stderr, "[CALL] frame %llu: %s = 0x%08x (%d)\n", static_cast<unsigned long long>(frame),
                         call.text.c_str(), result, static_cast<int32_t>(result));
        }
        eax(s) = saved_eax;
        ecx(s) = saved_ecx;
        edx(s) = saved_edx;
    }
}

}
