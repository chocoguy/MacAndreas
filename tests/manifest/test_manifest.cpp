#include "check.hpp"

#include "macandreas/manifest.hpp"

#include <set>
#include <string>

using namespace ma::manifest;

namespace {

bool has_error(const std::vector<Diagnostic>& errors, std::string_view needle) {
    for (const auto& d : errors) {
        if (d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_parse_valid() {
    constexpr std::string_view text = R"(# native replacements
- addr: 0x00745560
  kind: func_replace
  source: native
  group: window
  target: ma_create_game_window   # trailing comment
  notes: "keeps # inside quotes"
- addr: 0x00400010
  kind: byte_patch
  source: silentpatch
  group: fix-a
  bytes: "90 90 c3"
  setting: fixes.a
- addr: 4194400
  kind: nop
  source: widescreen
  group: hud
  len: 6
  tso: true
)";
    auto m = parse(text, "sample.yaml");
    CHECK(m.has_value());
    if (!m) {
        for (const auto& d : m.error()) {
            std::fprintf(stderr, "%s\n", d.text().c_str());
        }
        return;
    }
    CHECK(m->entries.size() == 3);
    const Entry& a = m->entries[0];
    CHECK(a.addr == 0x00745560);
    CHECK(a.kind == Kind::func_replace);
    CHECK(a.source == Source::native);
    CHECK(a.target == "ma_create_game_window");
    CHECK(a.notes == "keeps # inside quotes");
    CHECK(a.line == 2);
    CHECK(!a.lift_time());
    const Entry& b = m->entries[1];
    CHECK((b.bytes == std::vector<uint8_t>{0x90, 0x90, 0xc3}));
    CHECK(b.span() == 3);
    CHECK(b.setting == "fixes.a");
    CHECK(b.lift_time());
    const Entry& c = m->entries[2];
    CHECK(c.addr == 4194400);
    CHECK(c.span() == 6);
    CHECK(c.tso);
}

void test_parse_errors() {
    auto m = parse(R"(- addr: 0x1000
  kind: teleport
  source: native
  group: g
- addr: 0x2000
  kind: nop
  source: native
- addr: zzz
  kind: byte_patch
  source: native
  group: g
  bytes: "9"
- addr: 0x3000
  kind: func_replace
  source: native
  group: g
  colour: red
  group: again
)",
                   "bad.yaml");
    CHECK(!m.has_value());
    if (m) {
        return;
    }
    CHECK(has_error(m.error(), "unknown kind \"teleport\""));
    CHECK(has_error(m.error(), "entry has no group"));
    CHECK(has_error(m.error(), "not a 32-bit address"));
    CHECK(has_error(m.error(), "even-length hex"));
    CHECK(has_error(m.error(), "unknown key \"colour\""));
    CHECK(has_error(m.error(), "duplicate key \"group\""));

    auto kinds = parse(R"(- addr: 0x10
  kind: nop
  source: native
  group: g
  bytes: "90"
- addr: 0x20
  kind: func_replace
  source: native
  group: g
- addr: 0x30
  kind: byte_patch
  source: native
  group: g
  bytes: "9090"
  len: 3
)",
                       "kinds.yaml");
    CHECK(!kinds.has_value());
    if (!kinds) {
        CHECK(has_error(kinds.error(), "nop takes only len"));
        CHECK(has_error(kinds.error(), "needs a target symbol"));
        CHECK(has_error(kinds.error(), "len 3 does not match 2 bytes"));
    }
}

void test_conflicts() {
    auto m = parse(R"(- addr: 0x00401000
  kind: func_replace
  source: native
  group: replace
  target: ma_native
- addr: 0x00401010
  kind: byte_patch
  source: silentpatch
  group: inside
  bytes: "90"
- addr: 0x00402000
  kind: nop
  source: widescreen
  group: first
  len: 4
- addr: 0x00402002
  kind: byte_patch
  source: project2dfx
  group: seeded-overlap
  bytes: "c3"
- addr: 0x00403001
  kind: call_redirect
  source: silentpatch
  group: misaligned
  target: ma_hook
- addr: 0x00404000
  kind: func_replace
  source: native
  group: not-a-function
  target: ma_other
)",
                   "conflicts.yaml");
    CHECK(m.has_value());
    if (!m) {
        return;
    }
    CheckContext ctx;
    std::set<uint32_t> starts = {0x00401000, 0x00401010, 0x00402000, 0x00402002, 0x00403000, 0x00404000};
    ctx.is_instruction_start = [&](uint32_t a) { return starts.contains(a); };
    ctx.is_function_entry = [](uint32_t a) { return a == 0x00401000; };
    ctx.function_end = [](uint32_t a) -> std::optional<uint32_t> {
        return a == 0x00401000 ? std::optional<uint32_t>(0x00401100) : std::nullopt;
    };
    ctx.is_call_instruction = [](uint32_t a) { return a == 0x00403000; };
    auto errors = check(*m, ctx);
    CHECK(has_error(errors, "byte_patch at 0x00402002 (project2dfx/seeded-overlap) overlaps nop at 0x00402000"));
    CHECK(has_error(errors, "call_redirect at 0x00403001 does not start on an instruction boundary"));
    CHECK(has_error(errors, "call_redirect at 0x00403001 is not a call instruction"));
    CHECK(has_error(errors, "func_replace at 0x00404000 is not a known function entry"));
    CHECK(has_error(errors, "silentpatch patch at 0x00401010 lands inside native func_replace 0x00401000"));
    CHECK(errors.size() == 5);

    auto clean = parse(R"(- addr: 0x00402000
  kind: nop
  source: widescreen
  group: first
  len: 2
- addr: 0x00402002
  kind: byte_patch
  source: project2dfx
  group: adjacent
  bytes: "c3"
)",
                       "clean.yaml");
    CHECK(clean.has_value() && check(*clean).empty());
}

}

void test_marker() {
    constexpr std::string_view good = R"(- addr: 0x00406560
  kind: marker
  source: native
  group: cdstream-tso
  tso: true
- addr: 0x00406560
  kind: func_replace
  source: native
  group: stream
  target: ma_stream
)";
    auto m = parse(good, "marker.yaml");
    CHECK(m.has_value());
    if (!m) {
        return;
    }
    CHECK(m->entries.size() == 2);
    CHECK(m->entries[0].kind == Kind::marker);
    CHECK(m->entries[0].tso);
    CHECK(m->entries[0].span() == 0);
    CHECK(!m->entries[0].lift_time());
    CheckContext context;
    context.is_function_entry = [](uint32_t addr) { return addr == 0x00406560; };
    CHECK(check(*m, context).empty());

    constexpr std::string_view bad = R"(- addr: 0x00406560
  kind: marker
  source: native
  group: cdstream-tso
- addr: 0x00406570
  kind: marker
  source: native
  group: cdstream-tso
  tso: true
  bytes: "90"
)";
    auto errors = parse(bad, "bad_marker.yaml");
    CHECK(!errors.has_value());
    if (errors) {
        return;
    }
    CHECK(has_error(errors.error(), "marker needs tso: true"));
    CHECK(has_error(errors.error(), "marker takes no bytes, target or len"));
}

void test_resume_addresses() {
    auto ok = parse(R"(- addr: 0x00401000
  kind: mid_hook
  source: widescreen
  group: g
  target: ws_hook
  resume: 0x00401006, 0x401010
)", "resume.yaml");
    CHECK(ok.has_value());
    CHECK(ok && ok->entries[0].resume == std::vector<uint32_t>({0x401006, 0x401010}));
    auto bad = parse(R"(- addr: 0x00401000
  kind: func_replace
  source: widescreen
  group: g
  target: ws_fn
  resume: 0x00401006
- addr: 0x00401020
  kind: mid_hook
  source: widescreen
  group: g
  target: ws_hook
  resume: somewhere
)", "resume.yaml");
    CHECK(!bad.has_value());
    CHECK(!bad && has_error(bad.error(), "func_replace takes no resume addresses"));
    CHECK(!bad && has_error(bad.error(), "resume \"somewhere\" is not a 32-bit address or an offset +1 to +15"));
    auto relative = parse(R"(- addr: 0x00401000, 0x00401020
  kind: mid_hook
  source: widescreen
  group: g
  target: ws_hook
  resume: +6
)", "resume.yaml");
    CHECK(relative.has_value());
    CHECK(relative && relative->entries.size() == 2);
    CHECK(relative && relative->entries[0].resume == std::vector<uint32_t>({0x401006}));
    CHECK(relative && relative->entries[1].resume == std::vector<uint32_t>({0x401026}));
    auto far = parse(R"(- addr: 0x00401000
  kind: mid_hook
  source: widescreen
  group: g
  target: ws_hook
  resume: +16
)", "resume.yaml");
    CHECK(!far && has_error(far.error(), "resume \"+16\" is not a 32-bit address or an offset +1 to +15"));
}

void test_vtable_redirect() {
    auto ok = parse(R"(- addr: 0x00871164
  kind: vtable_redirect
  source: silentpatch
  group: extra-animations
  target: sp_automobile_pre_render
)", "vtable.yaml");
    CHECK(ok.has_value());
    CHECK(ok && ok->entries[0].kind == ma::manifest::Kind::vtable_redirect);
    CHECK(ok && ok->entries[0].span() == 4 && !ok->entries[0].lift_time());
    auto bad = parse(R"(- addr: 0x00871164
  kind: vtable_redirect
  source: silentpatch
  group: extra-animations
  bytes: "00"
)", "vtable_bad.yaml");
    CHECK(!bad && has_error(bad.error(), "vtable_redirect needs a target symbol") &&
          has_error(bad.error(), "vtable_redirect takes no bytes or len"));
}

void test_imm_redirect() {
    auto ok = parse(R"(- addr: 0x00734127
  kind: imm_redirect
  source: silentpatch
  group: rotors
  target: sp_heli_rotor_alpha
)", "imm.yaml");
    CHECK(ok.has_value());
    CHECK(ok && ok->entries[0].kind == ma::manifest::Kind::imm_redirect);
    CHECK(ok && ok->entries[0].span() == 4 && ok->entries[0].lift_time());
    auto bad = parse(R"(- addr: 0x00734127
  kind: imm_redirect
  source: silentpatch
  group: rotors
  len: 4
)", "imm_bad.yaml");
    CHECK(!bad && has_error(bad.error(), "imm_redirect needs a target symbol") &&
          has_error(bad.error(), "imm_redirect takes no bytes or len"));
}

void test_operand_slots() {
    auto ok = parse(R"(- addr: 0x0058eb3f, 0x0058ec0c,
    0x0058f55c
  kind: operand_slot
  source: widescreen
  group: hud
  setting: widescreen.hud
  target: ws_hud_width
)", "slots.yaml");
    CHECK(ok.has_value());
    CHECK(ok && ok->entries.size() == 3);
    CHECK(ok && ok->entries[2].addr == 0x58f55c && ok->entries[2].target == "ws_hud_width");
    CHECK(ok && ok->entries[0].span() == 6 && ok->entries[0].lift_time());
    CHECK(ma::manifest::operand_slot_address(0x58eb3f) == 0xE8000000u + 4 * 0x58eb3fu);
    CHECK(ma::manifest::is_operand_slot_instruction(0xD8, 0x0D));
    CHECK(ma::manifest::is_operand_slot_instruction(0xD9, 0x05));
    CHECK(!ma::manifest::is_operand_slot_instruction(0xD9, 0x1D));
    CHECK(!ma::manifest::is_operand_slot_instruction(0xD8, 0x4C));
    auto bad = parse(R"(- addr: 0x00401000
  kind: operand_slot
  source: widescreen
  group: g
  bytes: 90
- addr: 0x02000000
  kind: operand_slot
  source: widescreen
  group: g
  target: ws_slot
- addr: 0x00401000, 0x00401010
  kind: mid_hook
  source: widescreen
  group: g
  target: ws_hook
  resume: 0x00401006
)", "slots.yaml");
    CHECK(!bad.has_value());
    CHECK(!bad && has_error(bad.error(), "operand_slot needs a target slot"));
    CHECK(!bad && has_error(bad.error(), "operand_slot takes no bytes or len"));
    CHECK(!bad && has_error(bad.error(), "operand_slot 0x02000000 is above 0x02000000"));
    CHECK(!bad && has_error(bad.error(), "an entry with several addresses takes no resume addresses"));
    auto data = parse(R"(- addr: 0x00866b78
  kind: data_slot
  source: widescreen
  group: hud
  target: ws_radar_width
)", "slots.yaml");
    CHECK(data && data->entries[0].span() == 4 && !data->entries[0].lift_time());
}

void test_groups_and_settings() {
    auto m = parse(R"(- addr: 0x00401000
  kind: data_poke
  source: silentpatch
  group: fix-a
  bytes: "90"
  setting: silentpatch.fix_a
- addr: 0x00401010
  kind: data_poke
  source: widescreen
  group: fix-a
  bytes: "90"
  setting: silentpatch.fix_a
- addr: 0x00401020
  kind: data_poke
  source: silentpatch
  group: fix-b
  bytes: "01"
  setting: silentpatch.fix_b
- addr: 0x00401030
  kind: data_poke
  source: silentpatch
  group: fix-b
  bytes: "01"
)",
                   "groups.yaml");
    CHECK(m.has_value());
    if (!m) {
        return;
    }
    CheckContext ctx;
    ctx.is_known_setting = [](std::string_view key) { return key == "silentpatch.fix_a"; };
    auto errors = check(*m, ctx);
    CHECK(has_error(errors, "group fix-a mixes sources: widescreen here, silentpatch at groups.yaml:1"));
    CHECK(has_error(errors, "group fix-b mixes settings: \"\" here, \"silentpatch.fix_b\" at groups.yaml:13"));
    CHECK(has_error(errors, "setting \"silentpatch.fix_b\" is not a known settings key"));
    CHECK(errors.size() == 3);

    auto lift = parse(R"(- addr: 0x00401000
  kind: byte_patch
  source: silentpatch
  group: fix-c
  bytes: "90"
  setting: silentpatch.fix_a
)",
                      "lift.yaml");
    CHECK(lift.has_value());
    if (lift) {
        auto lift_errors = check(*lift, ctx);
        CHECK(has_error(lift_errors, "lift-time patches cannot switch at run time"));
        CHECK(lift_errors.size() == 1);
    }
}

int main() {
    test_parse_valid();
    test_groups_and_settings();
    test_resume_addresses();
    test_operand_slots();
    test_vtable_redirect();
    test_imm_redirect();
    test_marker();
    test_parse_errors();
    test_conflicts();
    return ma::test::finish("manifest");
}
