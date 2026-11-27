#include "macandreas/settings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace ma::settings {

namespace {

constexpr std::string_view kKeys[] = {"key",   "type",  "default", "choices", "min",
                                      "max",   "step",  "title",   "notes",   "replaces"};

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
            if (c == '\\' && i + 1 < s.size()) {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '"') {
            quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

std::optional<std::string> unquote(std::string_view v) {
    if (v.empty() || v.front() != '"') {
        return std::string(v);
    }
    if (v.size() < 2 || v.back() != '"') {
        return std::nullopt;
    }
    std::string out;
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size()) {
            ++i;
        }
        out += v[i];
    }
    return out;
}

std::optional<int64_t> parse_int(std::string_view v) {
    int64_t out = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc() || ptr != v.data() + v.size() || v.empty()) {
        return std::nullopt;
    }
    return out;
}

std::optional<double> parse_number(std::string_view v) {
    double out = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc() || ptr != v.data() + v.size() || v.empty() || !std::isfinite(out)) {
        return std::nullopt;
    }
    return out;
}

std::vector<std::string> split_list(std::string_view v) {
    std::vector<std::string> out;
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
        v = v.substr(1, v.size() - 2);
    }
    size_t start = 0;
    while (start <= v.size()) {
        size_t comma = v.find(',', start);
        auto part = trim(v.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!part.empty()) {
            out.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

bool valid_key(std::string_view key) {
    if (key.empty() || key.find('.') == std::string_view::npos || key.front() == '.' || key.back() == '.') {
        return false;
    }
    return std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

void add(std::vector<Diagnostic>& out, const std::string& file, int line, std::string message) {
    out.push_back({file, line, std::move(message)});
}

struct Pending {
    std::map<std::string, std::string> fields;
    int line = 0;
};

void finish(const Pending& p, const std::string& file, Schema& schema, std::vector<Diagnostic>& errors) {
    auto field = [&](std::string_view k) -> const std::string* {
        auto it = p.fields.find(std::string(k));
        return it == p.fields.end() ? nullptr : &it->second;
    };
    size_t before = errors.size();
    Spec s;
    s.file = file;
    s.line = p.line;
    if (auto* v = field("key"); v && valid_key(*v)) {
        s.key = *v;
    } else {
        add(errors, file, p.line, v ? std::format("key \"{}\" must be lowercase words joined by dots", *v)
                                    : std::string("setting has no key"));
    }
    if (auto* v = field("type")) {
        if (*v == "bool") {
            s.type = Type::boolean;
        } else if (*v == "int") {
            s.type = Type::integer;
        } else if (*v == "number") {
            s.type = Type::number;
        } else if (*v == "choice") {
            s.type = Type::choice;
        } else {
            add(errors, file, p.line, std::format("unknown type \"{}\"", *v));
        }
    } else {
        add(errors, file, p.line, "setting has no type");
    }
    if (auto* v = field("choices")) {
        s.choices = split_list(*v);
    }
    if (s.type == Type::number) {
        for (auto [name, slot] :
             {std::pair{"min", &s.number_min}, std::pair{"max", &s.number_max}, std::pair{"step", &s.step}}) {
            if (auto* v = field(name)) {
                if (auto n = parse_number(*v)) {
                    *slot = *n;
                } else {
                    add(errors, file, p.line, std::format("{} \"{}\" is not a number", name, *v));
                }
            }
        }
    } else {
        for (auto [name, slot] : {std::pair{"min", &s.min}, std::pair{"max", &s.max}}) {
            if (auto* v = field(name)) {
                if (auto n = parse_int(*v)) {
                    *slot = *n;
                } else {
                    add(errors, file, p.line, std::format("{} \"{}\" is not an integer", name, *v));
                }
            }
        }
    }
    if (auto* v = field("title"); v && !v->empty()) {
        s.title = *v;
    } else {
        add(errors, file, p.line, "setting has no title");
    }
    if (auto* v = field("notes")) {
        s.notes = *v;
    }
    if (auto* v = field("replaces")) {
        s.replaces = *v;
    }
    if (s.type == Type::choice && s.choices.size() < 2) {
        add(errors, file, p.line, "choice needs at least two choices");
    }
    if (s.type != Type::choice && field("choices")) {
        add(errors, file, p.line, "only a choice takes choices");
    }
    if (s.type != Type::integer && s.type != Type::number && (field("min") || field("max"))) {
        add(errors, file, p.line, "only an int or a number takes min and max");
    }
    if (s.type != Type::number && field("step")) {
        add(errors, file, p.line, "only a number takes a step");
    }
    if (auto* v = field("default")) {
        s.default_value = *v;
        if (errors.size() == before) {
            if (auto problem = validate(s, *v); !problem.empty()) {
                add(errors, file, p.line, "default: " + problem);
            }
        }
    } else {
        add(errors, file, p.line, "setting has no default");
    }
    if (!s.key.empty() && schema.find(s.key)) {
        add(errors, file, p.line, std::format("duplicate setting \"{}\"", s.key));
    }
    if (errors.size() == before) {
        schema.specs.push_back(std::move(s));
    }
}

std::expected<std::string, std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot read {}", path.string()));
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}

std::string Diagnostic::text() const { return std::format("{}:{}: {}", file, line, message); }

const Spec* Schema::find(std::string_view key) const {
    for (const auto& s : specs) {
        if (s.key == key) {
            return &s;
        }
    }
    return nullptr;
}

std::string validate(const Spec& spec, std::string_view value) {
    switch (spec.type) {
    case Type::boolean:
        if (value != "true" && value != "false") {
            return std::format("{} must be true or false, not \"{}\"", spec.key, value);
        }
        return {};
    case Type::integer: {
        auto n = parse_int(value);
        if (!n) {
            return std::format("{} must be an integer, not \"{}\"", spec.key, value);
        }
        if (*n < spec.min || *n > spec.max) {
            return std::format("{} must be between {} and {}, not {}", spec.key, spec.min, spec.max, *n);
        }
        return {};
    }
    case Type::number: {
        auto n = parse_number(value);
        if (!n) {
            return std::format("{} must be a number, not \"{}\"", spec.key, value);
        }
        if (*n < spec.number_min || *n > spec.number_max) {
            return std::format("{} must be between {} and {}, not {}", spec.key, spec.number_min, spec.number_max, *n);
        }
        return {};
    }
    case Type::choice:
        if (std::ranges::find(spec.choices, value) == spec.choices.end()) {
            std::string list;
            for (const auto& c : spec.choices) {
                list += (list.empty() ? "" : ", ") + c;
            }
            return std::format("{} must be one of {}, not \"{}\"", spec.key, list, value);
        }
        return {};
    }
    return "unknown type";
}

std::expected<Schema, std::vector<Diagnostic>> parse_schema(std::string_view text, const std::string& file) {
    Schema schema;
    std::vector<Diagnostic> errors;
    std::optional<Pending> current;
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
        if (body.starts_with("- ")) {
            if (indent != 0) {
                add(errors, file, line_no, "settings must start at column 1 with \"- \"");
                continue;
            }
            if (current) {
                finish(*current, file, schema, errors);
            }
            current.emplace();
            current->line = line_no;
            body = trim(body.substr(2));
        } else if (!current) {
            add(errors, file, line_no, "expected a list entry starting with \"- \"");
            continue;
        } else if (indent == 0) {
            add(errors, file, line_no, "setting fields must be indented");
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
            add(errors, file, line_no, std::format("unknown field \"{}\"", key));
            continue;
        }
        if (!value) {
            add(errors, file, line_no, std::format("unterminated quote in \"{}\"", key));
            continue;
        }
        if (!current->fields.emplace(key, *value).second) {
            add(errors, file, line_no, std::format("duplicate field \"{}\"", key));
        }
    }
    if (current) {
        finish(*current, file, schema, errors);
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return schema;
}

std::expected<Schema, std::vector<Diagnostic>> load_schema(const std::filesystem::path& path) {
    auto text = read_file(path);
    if (!text) {
        return std::unexpected(std::vector<Diagnostic>{{path.string(), 0, text.error()}});
    }
    return parse_schema(*text, path.filename().string());
}

const Spec& Values::spec(std::string_view key) const {
    const Spec* s = schema_ ? schema_->find(key) : nullptr;
    if (!s) {
        throw std::logic_error(std::format("setting \"{}\" is not in the schema", key));
    }
    return *s;
}

std::expected<void, std::string> Values::set(std::string_view key, std::string_view value) {
    const Spec* s = schema_ ? schema_->find(key) : nullptr;
    if (!s) {
        return std::unexpected(std::format("unknown setting \"{}\"", key));
    }
    if (auto problem = validate(*s, value); !problem.empty()) {
        return std::unexpected(problem);
    }
    values_.insert_or_assign(std::string(key), std::string(value));
    return {};
}

std::string Values::raw(std::string_view key) const {
    const Spec& s = spec(key);
    auto it = values_.find(key);
    return it == values_.end() ? s.default_value : it->second;
}

bool Values::flag(std::string_view key) const {
    if (spec(key).type != Type::boolean) {
        throw std::logic_error(std::format("setting \"{}\" is not a bool", key));
    }
    return raw(key) == "true";
}

int64_t Values::integer(std::string_view key) const {
    if (spec(key).type != Type::integer) {
        throw std::logic_error(std::format("setting \"{}\" is not an int", key));
    }
    return *parse_int(raw(key));
}

double Values::number(std::string_view key) const {
    if (spec(key).type != Type::number) {
        throw std::logic_error(std::format("setting \"{}\" is not a number", key));
    }
    return *parse_number(raw(key));
}

std::string Values::choice(std::string_view key) const {
    if (spec(key).type != Type::choice) {
        throw std::logic_error(std::format("setting \"{}\" is not a choice", key));
    }
    return raw(key);
}

bool Values::is_default(std::string_view key) const { return raw(key) == spec(key).default_value; }

std::string Values::serialize() const {
    std::string out;
    if (!schema_) {
        return out;
    }
    for (const auto& s : schema_->specs) {
        out += std::format("{} = {}\n", s.key, raw(s.key));
    }
    return out;
}

std::expected<Values, std::vector<Diagnostic>> parse_values(std::string_view text, const std::string& file,
                                                            const Schema& schema) {
    Values values(&schema);
    std::vector<Diagnostic> errors;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string_view raw = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++line_no;
        std::string_view line = trim(strip_comment(raw));
        if (line.empty()) {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            add(errors, file, line_no, std::format("expected \"key = value\", got \"{}\"", line));
            continue;
        }
        auto key = trim(line.substr(0, eq));
        auto value = trim(line.substr(eq + 1));
        if (auto r = values.set(key, value); !r) {
            add(errors, file, line_no, r.error());
        }
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return values;
}

std::expected<Values, std::vector<Diagnostic>> load_values(const std::filesystem::path& path, const Schema& schema) {
    auto text = read_file(path);
    if (!text) {
        return std::unexpected(std::vector<Diagnostic>{{path.string(), 0, text.error()}});
    }
    return parse_values(*text, path.filename().string(), schema);
}

}
