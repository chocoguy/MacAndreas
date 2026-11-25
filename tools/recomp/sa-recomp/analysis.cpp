#include "analysis.hpp"

#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

namespace ma::recomp {

namespace {

std::expected<llvm::json::Value, std::string> read_json(const std::filesystem::path& path) {
    auto buffer = llvm::MemoryBuffer::getFile(path.string());
    if (!buffer) {
        return std::unexpected(std::format("cannot read {}: {}", path.string(), buffer.getError().message()));
    }
    auto value = llvm::json::parse((*buffer)->getBuffer());
    if (!value) {
        return std::unexpected(std::format("cannot parse {}: {}", path.string(), llvm::toString(value.takeError())));
    }
    return std::move(*value);
}

std::optional<uint32_t> parse_address(std::optional<llvm::StringRef> text) {
    if (!text || !text->starts_with("0x")) {
        return std::nullopt;
    }
    uint32_t value = 0;
    auto digits = text->drop_front(2);
    auto [ptr, ec] = std::from_chars(digits.begin(), digits.end(), value, 16);
    if (ec != std::errc{} || ptr != digits.end()) {
        return std::nullopt;
    }
    return value;
}

}

std::expected<Analysis, std::string> load_analysis(const std::filesystem::path& dir) {
    Analysis analysis;

    auto meta = read_json(dir / "meta.json");
    if (!meta) {
        return std::unexpected(meta.error());
    }
    if (auto* obj = meta->getAsObject()) {
        if (auto sha = obj->getString("exe_sha256")) {
            analysis.exe_sha256 = sha->str();
        }
    }

    auto functions = read_json(dir / "functions.json");
    if (!functions) {
        return std::unexpected(functions.error());
    }
    auto* fobj = functions->getAsObject();
    auto* farr = fobj ? fobj->getArray("functions") : nullptr;
    if (!farr) {
        return std::unexpected("functions.json has no functions array");
    }
    for (const auto& item : *farr) {
        auto* o = item.getAsObject();
        auto entry = o ? parse_address(o->getString("entry")) : std::nullopt;
        if (!entry) {
            return std::unexpected("functions.json has an entry without a valid address");
        }
        auto section = o->getString("section");
        uint32_t end = 0;
        if (auto* ranges = o->getArray("ranges")) {
            for (const auto& range : *ranges) {
                auto* pair = range.getAsArray();
                if (pair && pair->size() == 2) {
                    if (auto last = parse_address((*pair)[1].getAsString())) {
                        end = std::max(end, *last + 1);
                    }
                }
            }
        }
        analysis.functions.push_back({*entry, end, section ? section->str() : std::string()});
    }

    auto jumps = read_json(dir / "jumptables.json");
    if (!jumps) {
        return std::unexpected(jumps.error());
    }
    auto* jobj = jumps->getAsObject();
    auto* jarr = jobj ? jobj->getArray("computed_jumps") : nullptr;
    if (!jarr) {
        return std::unexpected("jumptables.json has no computed_jumps array");
    }
    for (const auto& item : *jarr) {
        auto* o = item.getAsObject();
        auto site = o ? parse_address(o->getString("site")) : std::nullopt;
        auto* targets = o ? o->getArray("targets") : nullptr;
        if (!site) {
            continue;
        }
        if (!targets || targets->empty()) {
            analysis.unresolved_jumps.push_back(*site);
            continue;
        }
        auto& list = analysis.jump_targets[*site];
        for (const auto& t : *targets) {
            if (auto addr = parse_address(t.getAsString())) {
                list.push_back(*addr);
            }
        }
        std::ranges::sort(list);
    }
    std::ifstream extra(dir / "extra_roots.txt");
    for (std::string line; std::getline(extra, line);) {
        auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (auto addr = parse_address(llvm::StringRef(line))) {
            analysis.extra_roots.push_back(*addr);
        } else {
            return std::unexpected(std::format("extra_roots.txt: \"{}\" is not an address", line));
        }
    }
    return analysis;
}

}
