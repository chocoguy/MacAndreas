#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ma::settings {

enum class Type { boolean, integer, number, choice };

struct Spec {
    std::string key;
    Type type = Type::boolean;
    std::string default_value;
    std::vector<std::string> choices;
    int64_t min = INT64_MIN;
    int64_t max = INT64_MAX;
    double number_min = -1e300;
    double number_max = 1e300;
    double step = 0;
    std::string title;
    std::string notes;
    std::string replaces;
    std::string file;
    int line = 0;
};

struct Diagnostic {
    std::string file;
    int line = 0;
    std::string message;

    std::string text() const;
};

struct Schema {
    std::vector<Spec> specs;

    const Spec* find(std::string_view key) const;
};

std::expected<Schema, std::vector<Diagnostic>> parse_schema(std::string_view text, const std::string& file);
std::expected<Schema, std::vector<Diagnostic>> load_schema(const std::filesystem::path& path);

std::string validate(const Spec& spec, std::string_view value);

class Values {
public:
    explicit Values(const Schema* schema = nullptr) : schema_(schema) {}

    std::expected<void, std::string> set(std::string_view key, std::string_view value);
    std::string raw(std::string_view key) const;
    bool flag(std::string_view key) const;
    int64_t integer(std::string_view key) const;
    double number(std::string_view key) const;
    std::string choice(std::string_view key) const;
    bool is_default(std::string_view key) const;
    const Schema* schema() const { return schema_; }
    std::string serialize() const;

private:
    const Spec& spec(std::string_view key) const;

    const Schema* schema_;
    std::map<std::string, std::string, std::less<>> values_;
};

std::expected<Values, std::vector<Diagnostic>> parse_values(std::string_view text, const std::string& file,
                                                            const Schema& schema);
std::expected<Values, std::vector<Diagnostic>> load_values(const std::filesystem::path& path, const Schema& schema);

}
