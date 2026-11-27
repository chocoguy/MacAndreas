#include "macandreas/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace ma::manifest {

namespace {

constexpr std::pair<std::string_view, Kind> kKinds[] = {
    {"byte_patch", Kind::byte_patch},       {"nop", Kind::nop},           {"data_poke", Kind::data_poke},
    {"call_redirect", Kind::call_redirect}, {"mid_hook", Kind::mid_hook}, {"func_replace", Kind::func_replace},
    {"operand_slot", Kind::operand_slot}, {"data_slot", Kind::data_slot},       {"marker", Kind::marker},
};

constexpr std::pair<std::string_view, Source> kSources[] = {
    {"silentpatch", Source::silentpatch},
    {"widescreen", Source::widescreen},
    {"project2dfx", Source::project2dfx},
    {"native", Source::native},
};

constexpr std::string_view kKeys[] = {"addr", "kind",    "source", "group", "target", "bytes",
                                      "len",  "tso",     "setting", "resume", "site", "notes"};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view strip_comment(std::string_view s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

std::optional<std::string> unquote(std::string_view v) {
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
        return std::string(v.substr(1, v.size() - 2));
    }
    if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
        return std::nullopt;
    }
    return std::string(v);
}

std::optional<uint64_t> parse_integer(std::string_view v) {
    uint64_t out = 0;
    int base = 10;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v.remove_prefix(2);
        base = 16;
    }
    if (v.empty()) {
        return std::nullopt;
    }
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
    if (ec != std::errc() || ptr != v.data() + v.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<uint8_t>> parse_bytes(std::string_view v) {
    std::string digits;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        digits += c;
    }
    if (digits.empty() || digits.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<uint8_t> out;
    for (size_t i = 0; i < digits.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::vector<std::string> split_list(std::string_view list) {
    std::vector<std::string> out;
    while (!list.empty()) {
        auto comma = list.find(',');
        std::string item(trim(list.substr(0, comma)));
        if (!item.empty() || comma != std::string_view::npos) {
            out.push_back(std::move(item));
        }
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return out;
}

struct Pending {
    Entry entry;
    std::map<std::string, std::string> fields;
};

void add(std::vector<Diagnostic>& out, const std::string& file, int line, std::string message) {
    out.push_back({file, line, std::move(message)});
}

void finish_entry(Pending& p, std::vector<Diagnostic>& errors, Manifest& manifest) {
    Entry& e = p.entry;
    const std::string& f = e.file;
    int line = e.line;
    auto field = [&](std::string_view key) -> const std::string* {
        auto it = p.fields.find(std::string(key));
        return it == p.fields.end() ? nullptr : &it->second;
    };
    size_t before = errors.size();

    std::vector<uint32_t> addrs;
    if (auto* v = field("addr")) {
        for (const auto& item : split_list(*v)) {
            auto n = parse_integer(item);
            if (!n || *n > 0xffffffffull) {
                add(errors, f, line, std::format("addr \"{}\" is not a 32-bit address", item));
                break;
            }
            addrs.push_back(static_cast<uint32_t>(*n));
        }
        if (addrs.empty() && errors.size() == before) {
            add(errors, f, line, "entry has no addr");
        }
    } else {
        add(errors, f, line, "entry has no addr");
    }
    if (auto* v = field("kind")) {
        auto it = std::ranges::find(kKinds, *v, &std::pair<std::string_view, Kind>::first);
        if (it == std::end(kKinds)) {
            add(errors, f, line, std::format("unknown kind \"{}\"", *v));
        } else {
            e.kind = it->second;
        }
    } else {
        add(errors, f, line, "entry has no kind");
    }
    if (auto* v = field("source")) {
        auto it = std::ranges::find(kSources, *v, &std::pair<std::string_view, Source>::first);
        if (it == std::end(kSources)) {
            add(errors, f, line, std::format("unknown source \"{}\"", *v));
        } else {
            e.source = it->second;
        }
    } else {
        add(errors, f, line, "entry has no source");
    }
    if (auto* v = field("group"); v && !v->empty()) {
        e.group = *v;
    } else {
        add(errors, f, line, "entry has no group");
    }
    if (auto* v = field("target")) {
        e.target = *v;
    }
    if (auto* v = field("bytes")) {
        auto b = parse_bytes(*v);
        if (!b) {
            add(errors, f, line, std::format("bytes \"{}\" is not an even-length hex string", *v));
        } else {
            e.bytes = std::move(*b);
        }
    }
    if (auto* v = field("len")) {
        auto n = parse_integer(*v);
        if (!n || *n == 0 || *n > 0x10000) {
            add(errors, f, line, std::format("len \"{}\" is not a positive length", *v));
        } else {
            e.len = static_cast<uint32_t>(*n);
        }
    }
    if (auto* v = field("tso")) {
        if (*v == "true") {
            e.tso = true;
        } else if (*v != "false") {
            add(errors, f, line, std::format("tso \"{}\" is not true or false", *v));
        }
    }
    if (auto* v = field("setting")) {
        e.setting = *v;
    }
    std::vector<uint32_t> resume_offsets;
    if (auto* v = field("resume")) {
        for (const auto& item : split_list(*v)) {
            std::string_view text = item;
            bool relative = text.starts_with('+');
            auto n = parse_integer(relative ? text.substr(1) : text);
            if (!n || *n > 0xffffffffull || (relative && (*n == 0 || *n > 15))) {
                add(errors, f, line,
                    std::format("resume \"{}\" is not a 32-bit address or an offset +1 to +15", item));
                break;
            }
            (relative ? resume_offsets : e.resume).push_back(static_cast<uint32_t>(*n));
        }
    }
    if (auto* v = field("site")) {
        e.site = *v;
    }
    if (auto* v = field("notes")) {
        e.notes = *v;
    }
    if (errors.size() != before) {
        return;
    }

    bool has_bytes = !e.bytes.empty();
    bool has_target = !e.target.empty();
    if ((!e.resume.empty() || !resume_offsets.empty()) && e.kind != Kind::mid_hook) {
        add(errors, f, line, std::format("{} takes no resume addresses", to_string(e.kind)));
    }
    if (addrs.size() > 1 && !e.resume.empty()) {
        add(errors, f, line, "an entry with several addresses takes no resume addresses");
    }
    if (addrs.size() > 1 && !e.site.empty()) {
        add(errors, f, line, "an entry with several addresses takes no site");
    }
    switch (e.kind) {
    case Kind::byte_patch:
    case Kind::data_poke:
        if (!has_bytes) {
            add(errors, f, line, std::format("{} needs bytes", to_string(e.kind)));
        }
        if (e.len != 0 && e.len != e.bytes.size()) {
            add(errors, f, line, std::format("len {} does not match {} bytes", e.len, e.bytes.size()));
        }
        if (has_target) {
            add(errors, f, line, std::format("{} takes no target", to_string(e.kind)));
        }
        break;
    case Kind::nop:
        if (e.len == 0) {
            add(errors, f, line, "nop needs len");
        }
        if (has_bytes || has_target) {
            add(errors, f, line, "nop takes only len");
        }
        break;
    case Kind::call_redirect:
    case Kind::mid_hook:
    case Kind::func_replace:
        if (!has_target) {
            add(errors, f, line, std::format("{} needs a target symbol", to_string(e.kind)));
        }
        if (has_bytes) {
            add(errors, f, line, std::format("{} takes no bytes", to_string(e.kind)));
        }
        if (e.kind == Kind::call_redirect && e.len != 0 && e.len != 5 && e.len != 6) {
            add(errors, f, line, "call_redirect len must be 5 (call rel32) or 6 (call [mem])");
        }
        break;
    case Kind::operand_slot:
    case Kind::data_slot:
        if (!has_target) {
            add(errors, f, line, std::format("{} needs a target slot", to_string(e.kind)));
        }
        if (has_bytes || e.len != 0) {
            add(errors, f, line, std::format("{} takes no bytes or len", to_string(e.kind)));
        }
        for (uint32_t a : addrs) {
            if (e.kind == Kind::operand_slot && a >= kOperandSlotLimit) {
                add(errors, f, line, std::format("operand_slot 0x{:08x} is above 0x{:08x}", a, kOperandSlotLimit));
            }
        }
        break;
    case Kind::marker:
        if (has_bytes || has_target || e.len != 0) {
            add(errors, f, line, "marker takes no bytes, target or len");
        }
        if (!e.tso) {
            add(errors, f, line, "marker needs tso: true");
        }
        break;
    }
    if (errors.size() == before) {
        for (uint32_t a : addrs) {
            Entry copy = e;
            copy.addr = a;
            for (uint32_t offset : resume_offsets) {
                copy.resume.push_back(a + offset);
            }
            manifest.entries.push_back(std::move(copy));
        }
    }
}

}

std::string_view to_string(Kind kind) {
    for (auto& [name, k] : kKinds) {
        if (k == kind) {
            return name;
        }
    }
    return "?";
}

std::string_view to_string(Source source) {
    for (auto& [name, s] : kSources) {
        if (s == source) {
            return name;
        }
    }
    return "?";
}

uint32_t Entry::span() const {
    switch (kind) {
    case Kind::byte_patch:
    case Kind::data_poke: return static_cast<uint32_t>(bytes.size());
    case Kind::nop: return len;
    case Kind::call_redirect:
    case Kind::mid_hook: return len ? len : 5;
    case Kind::func_replace: return 1;
    case Kind::operand_slot: return 6;
    case Kind::data_slot: return 4;
    case Kind::marker: return 0;
    }
    return 1;
}

bool Entry::lift_time() const {
    return kind == Kind::byte_patch || kind == Kind::nop || kind == Kind::call_redirect || kind == Kind::mid_hook ||
           kind == Kind::operand_slot;
}

std::string Diagnostic::text() const { return std::format("{}:{}: {}", file, line, message); }

std::expected<Manifest, std::vector<Diagnostic>> parse(std::string_view text, const std::string& file) {
    Manifest manifest;
    std::vector<Diagnostic> errors;
    std::optional<Pending> current;
    std::string last_key;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string_view raw = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++line_no;
        std::string_view line = strip_comment(raw);
        if (trim(line).empty()) {
            continue;
        }
        size_t indent = line.find_first_not_of(" \t");
        std::string_view body = trim(line);
        if (body == "---" || body == "[]") {
            continue;
        }
        bool starts_entry = body.starts_with("- ") || body == "-";
        if (starts_entry) {
            if (indent != 0) {
                add(errors, file, line_no, "entries must start at column 1 with \"- \"");
                continue;
            }
            if (current) {
                finish_entry(*current, errors, manifest);
            }
            current.emplace();
            last_key.clear();
            current->entry.file = file;
            current->entry.line = line_no;
            body = trim(body.substr(1));
            if (body.empty()) {
                continue;
            }
        } else if (!current) {
            add(errors, file, line_no, "expected a list entry starting with \"- \"");
            continue;
        } else if (indent == 0) {
            add(errors, file, line_no, "entry fields must be indented");
            continue;
        }
        if (!starts_entry && !last_key.empty() && current->fields[last_key].ends_with(',')) {
            current->fields[last_key] += " ";
            current->fields[last_key] += body;
            continue;
        }
        size_t colon = body.find(':');
        if (colon == std::string_view::npos) {
            add(errors, file, line_no, std::format("expected \"key: value\", got \"{}\"", body));
            continue;
        }
        std::string key(trim(body.substr(0, colon)));
        auto value = unquote(trim(body.substr(colon + 1)));
        if (std::ranges::find(kKeys, key) == std::end(kKeys)) {
            add(errors, file, line_no, std::format("unknown key \"{}\"", key));
            continue;
        }
        if (!value) {
            add(errors, file, line_no, std::format("unterminated quote in \"{}\"", key));
            continue;
        }
        if (!current->fields.emplace(key, *value).second) {
            add(errors, file, line_no, std::format("duplicate key \"{}\"", key));
        }
        last_key = key;
    }
    if (current) {
        finish_entry(*current, errors, manifest);
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return manifest;
}

std::expected<Manifest, std::vector<Diagnostic>> load_directory(const std::filesystem::path& dir) {
    Manifest all;
    std::vector<Diagnostic> errors;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        auto ext = entry.path().extension();
        if (entry.is_regular_file() && (ext == ".yaml" || ext == ".yml")) {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        return std::unexpected(std::vector<Diagnostic>{{dir.string(), 0, ec.message()}});
    }
    std::ranges::sort(files);
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto parsed = parse(buffer.str(), path.filename().string());
        if (!parsed) {
            errors.insert(errors.end(), parsed.error().begin(), parsed.error().end());
            continue;
        }
        for (auto& e : parsed->entries) {
            all.entries.push_back(std::move(e));
        }
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return all;
}

std::vector<Diagnostic> check(const Manifest& manifest, const CheckContext& context) {
    std::vector<Diagnostic> out;
    std::vector<const Entry*> sorted;
    for (const auto& e : manifest.entries) {
        sorted.push_back(&e);
    }
    std::ranges::sort(sorted, [](const Entry* a, const Entry* b) {
        return a->addr != b->addr ? a->addr < b->addr : a->line < b->line;
    });
    for (size_t i = 0; i < sorted.size(); ++i) {
        const Entry& a = *sorted[i];
        uint64_t a_end = static_cast<uint64_t>(a.addr) + a.span();
        for (size_t j = i + 1; j < sorted.size() && sorted[j]->addr < a_end; ++j) {
            const Entry& b = *sorted[j];
            add(out, b.file, b.line,
                std::format("{} at 0x{:08x} ({}/{}) overlaps {} at 0x{:08x} ({}/{}) from {}:{}", to_string(b.kind),
                            b.addr, to_string(b.source), b.group, to_string(a.kind), a.addr, to_string(a.source),
                            a.group, a.file, a.line));
        }
    }
    for (const auto& e : manifest.entries) {
        bool code_patch = e.kind == Kind::byte_patch || e.kind == Kind::nop || e.kind == Kind::call_redirect ||
                          e.kind == Kind::mid_hook || e.kind == Kind::operand_slot;
        if (code_patch && context.is_instruction_start && !context.is_instruction_start(e.addr)) {
            add(out, e.file, e.line,
                std::format("{} at 0x{:08x} does not start on an instruction boundary", to_string(e.kind), e.addr));
        }
        if (e.kind == Kind::call_redirect && context.is_call_instruction && !context.is_call_instruction(e.addr)) {
            add(out, e.file, e.line, std::format("call_redirect at 0x{:08x} is not a call instruction", e.addr));
        }
        if (e.kind == Kind::func_replace && context.is_function_entry && !context.is_function_entry(e.addr)) {
            add(out, e.file, e.line, std::format("func_replace at 0x{:08x} is not a known function entry", e.addr));
        }
    }
    std::map<std::string, const Entry*> group_owner;
    for (const auto& e : manifest.entries) {
        auto [it, inserted] = group_owner.emplace(e.group, &e);
        if (inserted) {
            continue;
        }
        const Entry& first = *it->second;
        if (first.source != e.source) {
            add(out, e.file, e.line,
                std::format("group {} mixes sources: {} here, {} at {}:{}", e.group, to_string(e.source),
                            to_string(first.source), first.file, first.line));
        }
        if (first.setting != e.setting) {
            add(out, e.file, e.line,
                std::format("group {} mixes settings: \"{}\" here, \"{}\" at {}:{}", e.group, e.setting, first.setting,
                            first.file, first.line));
        }
    }
    for (const auto& e : manifest.entries) {
        if (!e.setting.empty() && (e.kind == Kind::byte_patch || e.kind == Kind::nop)) {
            add(out, e.file, e.line,
                std::format("{} at 0x{:08x} has setting \"{}\", but lift-time patches cannot switch at run time; "
                            "use data_poke, func_replace, call_redirect or mid_hook",
                            to_string(e.kind), e.addr, e.setting));
        }
    }
    if (context.is_known_setting) {
        for (const auto& e : manifest.entries) {
            if (!e.setting.empty() && !context.is_known_setting(e.setting)) {
                add(out, e.file, e.line, std::format("setting \"{}\" is not a known settings key", e.setting));
            }
        }
    }
    if (context.function_end) {
        for (const auto& owner : manifest.entries) {
            if (owner.kind != Kind::func_replace || owner.source != Source::native) {
                continue;
            }
            auto end = context.function_end(owner.addr);
            if (!end) {
                continue;
            }
            for (const auto& e : manifest.entries) {
                if (e.source != Source::native && e.addr >= owner.addr && e.addr < *end) {
                    add(out, e.file, e.line,
                        std::format("{} patch at 0x{:08x} lands inside native func_replace 0x{:08x} ({})",
                                    to_string(e.source), e.addr, owner.addr, owner.target));
                }
            }
        }
    }
    return out;
}

}
