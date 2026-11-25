#pragma once

#include <cstdio>
#include <source_location>

namespace ma::test {

inline int failures = 0;

inline void check(bool ok, const char* expr, std::source_location loc = std::source_location::current()) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "%s:%u: CHECK failed: %s\n", loc.file_name(), static_cast<unsigned>(loc.line()), expr);
    }
}

inline int finish(const char* name) {
    if (failures == 0) {
        std::fprintf(stdout, "%s: all checks passed\n", name);
        return 0;
    }
    std::fprintf(stderr, "%s: %d check(s) failed\n", name, failures);
    return 1;
}

inline constexpr int kSkip = 77;

}

#define CHECK(expr) ::ma::test::check(static_cast<bool>(expr), #expr)
