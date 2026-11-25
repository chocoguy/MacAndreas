#include "paths.hpp"

#include "process.hpp"

#include <cctype>
#include <mutex>
#include <system_error>
#include <vector>

namespace ma::host::win32 {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split(std::string_view path) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '\\') {
            if (!current.empty()) {
                parts.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::optional<std::string_view> strip_root(std::string_view path, std::string_view root) {
    if (path.size() < root.size() || !iequals(path.substr(0, root.size()), root)) {
        return std::nullopt;
    }
    std::string_view rest = path.substr(root.size());
    if (!rest.empty() && rest.front() != '\\') {
        return std::nullopt;
    }
    while (!rest.empty() && rest.front() == '\\') {
        rest.remove_prefix(1);
    }
    return rest;
}

}

std::string normalize_guest_path(std::string_view path, std::string_view cwd) {
    std::string p(path);
    for (char& c : p) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string absolute;
    if (p.size() >= 2 && p[1] == ':') {
        absolute = p;
    } else if (!p.empty() && p[0] == '\\') {
        absolute = std::string(cwd.substr(0, 2)) + p;
    } else {
        absolute = std::string(cwd) + "\\" + p;
    }
    std::string drive = absolute.substr(0, 2);
    drive[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(drive[0])));
    std::vector<std::string> out;
    for (auto& part : split(std::string_view(absolute).substr(2))) {
        if (part == ".") {
            continue;
        }
        if (part == "..") {
            if (!out.empty()) {
                out.pop_back();
            }
            continue;
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '.')) {
            part.pop_back();
        }
        if (!part.empty()) {
            out.push_back(part);
        }
    }
    std::string result = drive + "\\";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i != 0) {
            result += '\\';
        }
        result += out[i];
    }
    return result;
}

std::optional<std::filesystem::path> resolve_case(const std::filesystem::path& base, std::string_view relative) {
    std::filesystem::path current = base;
    auto parts = split(relative);
    for (size_t i = 0; i < parts.size(); ++i) {
        std::filesystem::path exact = current / parts[i];
        std::error_code ec;
        if (std::filesystem::exists(exact, ec)) {
            current = exact;
            continue;
        }
        bool found = false;
        for (std::filesystem::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec)) {
            if (iequals(it->path().filename().string(), parts[i])) {
                current = it->path();
                found = true;
                break;
            }
        }
        if (!found) {
            if (i + 1 != parts.size()) {
                return std::nullopt;
            }
            current = exact;
        }
    }
    return current;
}

namespace {

std::mutex g_cwd_mutex;
std::string g_cwd(kGuestGameRoot);

}

std::string current_directory() {
    std::lock_guard lock(g_cwd_mutex);
    return g_cwd;
}

void set_current_directory(std::string guest_path) {
    std::lock_guard lock(g_cwd_mutex);
    g_cwd = std::move(guest_path);
}

std::optional<std::filesystem::path> host_path_for(std::string_view guest_path) {
    std::string normalized = normalize_guest_path(guest_path, current_directory());
    if (auto rest = strip_root(normalized, kGuestGameRoot)) {
        return resolve_case(process().game_dir, *rest);
    }
    if (auto rest = strip_root(normalized, kGuestDocuments)) {
        return resolve_case(process().user_dir / "Documents", *rest);
    }
    return std::nullopt;
}

bool wildcard_match(std::string_view pattern, std::string_view name) {
    if (pattern == "*.*" || pattern == "*") {
        return true;
    }
    size_t p = 0;
    size_t n = 0;
    size_t star = std::string_view::npos;
    size_t mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '.')) {
        ++p;
    }
    return p == pattern.size();
}

}
