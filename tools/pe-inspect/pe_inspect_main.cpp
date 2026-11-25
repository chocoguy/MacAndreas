#include "json_writer.hpp"

#include "macandreas/pe_image.hpp"

#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string perm(const ma::pe::Section& s) {
    return std::string(s.readable() ? "r" : "-") + (s.writable() ? "w" : "-") + (s.executable() ? "x" : "-");
}

std::string import_label(const ma::pe::ImportModule& m, const ma::pe::ImportEntry& e) {
    if (!e.ordinal) {
        return e.name;
    }
    auto known = ma::pe::known_ordinal_name(m.dll, *e.ordinal);
    return known ? std::format("#{} ({})", *e.ordinal, *known) : std::format("#{}", *e.ordinal);
}

void print_text(const ma::pe::Image& img) {
    std::println("machine        0x{:04x}", img.machine);
    std::println("timestamp      0x{:08x}", img.timestamp);
    std::println("image base     0x{:08x}", img.image_base);
    std::println("entry point    0x{:08x}", img.entry_va());
    std::println("size of image  0x{:x}", img.size_of_image);
    std::println("subsystem      {}", img.subsystem);
    auto tls = img.directory(ma::pe::Directory::tls);
    auto reloc = img.directory(ma::pe::Directory::relocations);
    std::println("tls directory  {}", tls.rva ? std::format("0x{:08x}", img.image_base + tls.rva) : "none");
    std::println("relocations    {}", reloc.rva ? std::format("{} bytes", reloc.size) : "none");
    std::println("");
    std::println("{:<10} {:>10} {:>10} {:>10} {:>10} {}", "section", "va", "vsize", "raw", "rawsize", "perm");
    for (const auto& s : img.sections) {
        std::println("{:<10} 0x{:08x} 0x{:08x} 0x{:08x} 0x{:08x} {}", s.name, img.image_base + s.virtual_address,
                     s.virtual_size, s.raw_offset, s.raw_size, perm(s));
    }
    std::println("");
    for (const auto& m : img.imports) {
        std::println("{} ({})", m.dll, m.entries.size());
        for (const auto& e : m.entries) {
            std::println("  0x{:08x}  {}", img.image_base + e.iat_rva, import_label(m, e));
        }
    }
    std::println("");
    std::println("total imports {} from {} modules", img.import_count(), img.imports.size());
}

void print_json(const ma::pe::Image& img) {
    using ma::tools::JsonObject;
    std::vector<std::string> sections;
    for (const auto& s : img.sections) {
        sections.push_back(JsonObject()
                               .str("name", s.name)
                               .hex("va", img.image_base + s.virtual_address)
                               .hex("vsize", s.virtual_size)
                               .hex("raw_offset", s.raw_offset)
                               .hex("raw_size", s.raw_size)
                               .str("perm", perm(s))
                               .build());
    }
    std::vector<std::string> modules;
    for (const auto& m : img.imports) {
        std::vector<std::string> entries;
        for (const auto& e : m.entries) {
            JsonObject o;
            o.hex("iat", img.image_base + e.iat_rva);
            if (e.ordinal) {
                o.num("ordinal", *e.ordinal);
                if (auto known = ma::pe::known_ordinal_name(m.dll, *e.ordinal)) {
                    o.str("known_name", *known);
                }
            } else {
                o.str("name", e.name);
            }
            entries.push_back(o.build());
        }
        modules.push_back(JsonObject().str("dll", m.dll).raw("entries", ma::tools::json_array(entries)).build());
    }
    std::println("{}", JsonObject()
                           .hex("machine", img.machine)
                           .hex("timestamp", img.timestamp)
                           .hex("image_base", img.image_base)
                           .hex("entry_point", img.entry_va())
                           .hex("size_of_image", img.size_of_image)
                           .num("import_count", img.import_count())
                           .raw("sections", ma::tools::json_array(sections))
                           .raw("imports", ma::tools::json_array(modules))
                           .build());
}

}

int main(int argc, char** argv) {
    bool json = false;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--json") {
            json = true;
        } else if (path.empty()) {
            path = arg;
        } else {
            path.clear();
            break;
        }
    }
    if (path.empty()) {
        std::println(stderr, "usage: pe-inspect [--json] <file.exe>");
        return 64;
    }
    auto bytes = ma::pe::read_file(path);
    if (!bytes) {
        std::println(stderr, "pe-inspect: {}", bytes.error());
        return 1;
    }
    auto img = ma::pe::parse(*bytes);
    if (!img) {
        std::println(stderr, "pe-inspect: {}: {}", path, img.error());
        return 1;
    }
    if (json) {
        print_json(*img);
    } else {
        print_text(*img);
    }
    return 0;
}
