#include "check.hpp"

#include "macandreas/exe_identity.hpp"
#include "macandreas/pe_image.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace {

struct ExpectedSection {
    std::string_view name;
    uint32_t va;
    uint32_t vsize;
    bool write;
    bool exec;
};

constexpr std::array kSections = {
    ExpectedSection{".text", 0x00401000, 0x456000, false, true},
    ExpectedSection{"_rwcseg", 0x00857000, 0x1000, false, true},
    ExpectedSection{".rdata", 0x00858000, 0x4c000, false, false},
    ExpectedSection{".data", 0x008a4000, 0x3fa000, true, false},
    ExpectedSection{"_TEXT_HA", 0x00c9e000, 0x11000, true, false},
    ExpectedSection{"_rwdseg", 0x00caf000, 0x1000, true, false},
    ExpectedSection{".rsrc", 0x00cb0000, 0x1000, false, false},
    ExpectedSection{".text", 0x00cb1000, 0x64a000, true, true},
    ExpectedSection{".init", 0x012fb000, 0x6000, true, true},
    ExpectedSection{".data", 0x01301000, 0x255000, true, true},
    ExpectedSection{".HOODLUM", 0x01556000, 0x21000, true, true},
};

struct ExpectedModule {
    std::string_view dll;
    size_t count;
};

constexpr std::array kModules = {
    ExpectedModule{"WINMM.dll", 4},     ExpectedModule{"vorbisfile.dll", 7}, ExpectedModule{"WS2_32.dll", 10},
    ExpectedModule{"EAX.DLL", 1},       ExpectedModule{"KERNEL32.dll", 138}, ExpectedModule{"USER32.dll", 44},
    ExpectedModule{"GDI32.dll", 1},     ExpectedModule{"ADVAPI32.dll", 6},   ExpectedModule{"ole32.dll", 3},
};

const ma::pe::ImportModule* find_module(const ma::pe::Image& img, std::string_view dll) {
    for (const auto& m : img.imports) {
        if (m.dll == dll) {
            return &m;
        }
    }
    return nullptr;
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_game_exe <game dir>\n");
        return 1;
    }
    std::filesystem::path game_dir = argv[1];
    auto exe = ma::exe::locate_exe(game_dir / "gta_sa.exe");
    if (!exe) {
        std::fprintf(stdout, "skipping: no gta_sa.exe in %s\n", game_dir.string().c_str());
        return ma::test::kSkip;
    }
    auto bytes = ma::pe::read_file(*exe);
    CHECK(bytes.has_value());
    if (!bytes) {
        return ma::test::finish("test_game_exe");
    }

    auto id = ma::exe::identify(*bytes);
    CHECK(id.support == ma::exe::Support::supported);
    CHECK(id.build == "1.0 US");
    CHECK(id.sha256 == ma::exe::kSupportedSha256);
    if (id.support != ma::exe::Support::supported) {
        std::fprintf(stderr, "this test needs the 1.0 US exe; got %s\n", id.sha256.c_str());
        return ma::test::finish("test_game_exe");
    }

    auto img = ma::pe::parse(*bytes);
    CHECK(img.has_value());
    if (!img) {
        return ma::test::finish("test_game_exe");
    }
    CHECK(img->image_base == 0x00400000);
    CHECK(img->entry_va() == 0x00824570);
    CHECK(img->directory(ma::pe::Directory::tls).rva == 0);
    CHECK(img->sections.size() == kSections.size());
    for (size_t i = 0; i < kSections.size() && i < img->sections.size(); ++i) {
        const auto& s = img->sections[i];
        const auto& e = kSections[i];
        CHECK(s.name == e.name);
        CHECK(img->image_base + s.virtual_address == e.va);
        CHECK(s.virtual_size == e.vsize);
        CHECK(s.writable() == e.write);
        CHECK(s.executable() == e.exec);
    }

    CHECK(img->import_count() == 214);
    CHECK(img->imports.size() == kModules.size());
    for (const auto& expected : kModules) {
        const auto* m = find_module(*img, expected.dll);
        CHECK(m != nullptr);
        if (m) {
            CHECK(m->entries.size() == expected.count);
        }
    }
    if (const auto* eax = find_module(*img, "EAX.DLL"); eax && !eax->entries.empty()) {
        CHECK(eax->entries[0].ordinal == uint16_t{6});
    }
    if (const auto* ws = find_module(*img, "WS2_32.dll")) {
        for (const auto& e : ws->entries) {
            CHECK(e.ordinal.has_value());
            CHECK(e.ordinal && ma::pe::known_ordinal_name(ws->dll, *e.ordinal).has_value());
        }
    }

    auto modified = *bytes;
    modified[0x1000] ^= 0x01;
    auto mod_id = ma::exe::identify(modified);
    CHECK(mod_id.support == ma::exe::Support::unknown);
    CHECK(mod_id.build == "modified 1.0 US");

    auto steam = game_dir / "backups" / "gta-sa.exe";
    if (std::filesystem::exists(steam)) {
        auto steam_bytes = ma::pe::read_file(steam);
        CHECK(steam_bytes.has_value());
        if (steam_bytes) {
            auto steam_id = ma::exe::identify(*steam_bytes);
            CHECK(steam_id.support == ma::exe::Support::unsupported_known);
            CHECK(steam_id.build == "Steam release");
        }
    }

    return ma::test::finish("test_game_exe");
}
