#include "check.hpp"

#include "macandreas/exe_identity.hpp"
#include "macandreas/pe_image.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

class Builder {
public:
    explicit Builder(size_t size) : bytes(size, 0) {}

    void u16(size_t off, uint16_t v) {
        bytes[off] = static_cast<uint8_t>(v);
        bytes[off + 1] = static_cast<uint8_t>(v >> 8);
    }
    void u32(size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes[off + i] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    void str(size_t off, std::string_view s) { std::memcpy(bytes.data() + off, s.data(), s.size()); }

    std::vector<uint8_t> bytes;
};

constexpr size_t kPe = 0x40;
constexpr size_t kOpt = kPe + 24;
constexpr size_t kSections = kOpt + 0xe0;
constexpr size_t kIdataRaw = 0x400;
constexpr uint32_t kIdataRva = 0x2000;

size_t idata(uint32_t rva) { return kIdataRaw + (rva - kIdataRva); }

std::vector<uint8_t> make_pe() {
    Builder b(0x600);
    b.str(0, "MZ");
    b.u32(0x3c, kPe);
    b.str(kPe, std::string_view("PE\0\0", 4));
    b.u16(kPe + 4, 0x014c);
    b.u16(kPe + 6, 2);
    b.u32(kPe + 8, 0x12345678);
    b.u16(kPe + 20, 0xe0);
    b.u16(kPe + 22, 0x0102);

    b.u16(kOpt, 0x010b);
    b.u32(kOpt + 16, 0x1000);
    b.u32(kOpt + 28, 0x00400000);
    b.u32(kOpt + 32, 0x1000);
    b.u32(kOpt + 36, 0x200);
    b.u32(kOpt + 56, 0x3000);
    b.u32(kOpt + 60, 0x200);
    b.u16(kOpt + 68, 2);
    b.u32(kOpt + 92, 16);
    b.u32(kOpt + 96 + 8, kIdataRva);
    b.u32(kOpt + 96 + 12, 0x28);

    b.str(kSections, ".text");
    b.u32(kSections + 8, 0x10);
    b.u32(kSections + 12, 0x1000);
    b.u32(kSections + 16, 0x200);
    b.u32(kSections + 20, 0x200);
    b.u32(kSections + 36, 0x60000020);

    b.str(kSections + 40, ".idata");
    b.u32(kSections + 40 + 8, 0x200);
    b.u32(kSections + 40 + 12, kIdataRva);
    b.u32(kSections + 40 + 16, 0x200);
    b.u32(kSections + 40 + 20, kIdataRaw);
    b.u32(kSections + 40 + 36, 0xc0000040);

    b.u32(idata(0x2000), 0x2040);
    b.u32(idata(0x200c), 0x2080);
    b.u32(idata(0x2010), 0x2060);

    for (uint32_t table : {0x2040u, 0x2060u}) {
        b.u32(idata(table), 0x20a0);
        b.u32(idata(table + 4), 0x80000000u | 23);
    }
    b.str(idata(0x2080), "WS2_32.dll");
    b.u16(idata(0x20a0), 0x73);
    b.str(idata(0x20a2), "WSAStartup");
    return b.bytes;
}

void test_valid_image() {
    auto bytes = make_pe();
    auto img = ma::pe::parse(bytes);
    CHECK(img.has_value());
    if (!img) {
        return;
    }
    CHECK(img->machine == ma::pe::kMachineI386);
    CHECK(img->timestamp == 0x12345678);
    CHECK(img->image_base == 0x00400000);
    CHECK(img->entry_va() == 0x00401000);
    CHECK(img->sections.size() == 2);
    CHECK(img->sections[0].name == ".text");
    CHECK(img->sections[0].executable());
    CHECK(!img->sections[0].writable());
    CHECK(img->sections[1].name == ".idata");
    CHECK(img->sections[1].writable());
    CHECK(img->rva_to_offset(0x2080) == 0x480u);
    CHECK(img->rva_to_offset(0x100) == 0x100u);
    CHECK(!img->rva_to_offset(0x5000).has_value());
    CHECK(img->section_for_rva(0x1008) == &img->sections[0]);

    CHECK(img->imports.size() == 1);
    CHECK(img->import_count() == 2);
    if (img->imports.size() == 1 && img->imports[0].entries.size() == 2) {
        const auto& m = img->imports[0];
        CHECK(m.dll == "WS2_32.dll");
        CHECK(m.entries[0].name == "WSAStartup");
        CHECK(m.entries[0].hint == 0x73);
        CHECK(!m.entries[0].ordinal.has_value());
        CHECK(m.entries[0].iat_rva == 0x2060);
        CHECK(m.entries[1].ordinal == uint16_t{23});
        CHECK(m.entries[1].iat_rva == 0x2064);
    }
}

void test_malformed_images() {
    auto bytes = make_pe();

    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 0x30);
    CHECK(!ma::pe::parse(truncated).has_value());

    auto bad_sig = bytes;
    bad_sig[kPe] = 'X';
    CHECK(!ma::pe::parse(bad_sig).has_value());

    auto pe32plus = bytes;
    pe32plus[kOpt] = 0x0b;
    pe32plus[kOpt + 1] = 0x02;
    CHECK(!ma::pe::parse(pe32plus).has_value());

    auto amd64 = bytes;
    amd64[kPe + 4] = 0x64;
    amd64[kPe + 5] = 0x86;
    CHECK(!ma::pe::parse(amd64).has_value());

    auto bad_name = bytes;
    bad_name[idata(0x200c) + 1] = 0x7f;
    CHECK(!ma::pe::parse(bad_name).has_value());

    auto unterminated = bytes;
    std::memset(unterminated.data() + idata(0x2080), 'A', 0x600 - idata(0x2080));
    CHECK(!ma::pe::parse(unterminated).has_value());

    auto section_outside = bytes;
    section_outside[kSections + 40 + 21] = 0x7f;
    CHECK(!ma::pe::parse(section_outside).has_value());
}

void test_ordinal_names() {
    CHECK(ma::pe::known_ordinal_name("WS2_32.dll", 23) == std::string_view("socket"));
    CHECK(ma::pe::known_ordinal_name("ws2_32.DLL", 115) == std::string_view("WSAStartup"));
    CHECK(!ma::pe::known_ordinal_name("WS2_32.dll", 9999).has_value());
    CHECK(!ma::pe::known_ordinal_name("EAX.DLL", 6).has_value());
}

void test_sha256() {
    CHECK(ma::exe::sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char abc[] = "abc";
    CHECK(ma::exe::sha256_hex({reinterpret_cast<const uint8_t*>(abc), 3}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void test_identify_rejects_others() {
    auto bytes = make_pe();
    auto id = ma::exe::identify(bytes);
    CHECK(id.support == ma::exe::Support::unknown);
    CHECK(id.build == "unrecognized");
    CHECK(!id.message.empty());

    std::vector<uint8_t> text = {'h', 'e', 'l', 'l', 'o'};
    auto not_pe = ma::exe::identify(text);
    CHECK(not_pe.support == ma::exe::Support::unknown);
    CHECK(not_pe.build == "not a PE executable");
}

}

int main() {
    test_valid_image();
    test_malformed_images();
    test_ordinal_names();
    test_sha256();
    test_identify_rejects_others();
    return ma::test::finish("test_pe_synthetic");
}
