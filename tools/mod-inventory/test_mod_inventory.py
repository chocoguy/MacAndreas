import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mod_inventory as mi

SP_CFG = {
    "prefix": "sp",
    "repo": "https://example.invalid/sp",
    "commit": "0" * 40,
    "changelog": "",
    "files": [],
    "defines": ["_GTA_SA", "NDEBUG"],
    "versions": [
        {"scope": "_10$", "version": "1.0"},
        {"scope": "_11$", "version": "1.01"},
        {"scope": "Steam$", "version": "steam"},
        {"scope": "NewBinaries", "version": "newsteam"},
    ],
}
WS_CFG = dict(SP_CFG, prefix="ws", versions=[])

CODE_VA = 0x401000


def fake_image(code: bytes) -> mi.Image:
    data = bytearray(0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x40)
    data[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", data, 0x46, 1)
    struct.pack_into("<H", data, 0x54, 0xE0)
    opt = 0x58
    struct.pack_into("<H", data, opt, 0x10B)
    struct.pack_into("<I", data, opt + 4, len(code))
    struct.pack_into("<I", data, opt + 20, 0x1000)
    struct.pack_into("<I", data, opt + 28, 0x400000)
    sect = opt + 0xE0
    data[sect:sect + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, sect + 8, len(code), 0x1000, len(code), 0x400)
    struct.pack_into("<I", data, sect + 36, 0x60000020)
    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
        f.write(bytes(data) + code)
        path = Path(f.name)
    img = mi.Image.load(path)
    path.unlink()
    return img


def rows_for(cfg, text, image=None):
    sites = mi.scan_text(cfg, "src.cpp", text, image)
    rows, dropped = mi.make_rows(cfg["prefix"], sites)
    return rows, dropped


class CleanTests(unittest.TestCase):
    def test_comments_and_strings_are_masked_and_lines_kept(self):
        text = 'a = 1; // Patch(0x401000, 1);\n/* Nop(0x401000, 2);\n */ s = "Patch(0x1, 2)";\n'
        code, masked, comments = mi.clean(text)
        self.assertEqual(len(code), len(text))
        self.assertEqual(masked.count("\n"), text.count("\n"))
        self.assertNotIn("Patch", masked)
        self.assertIn('"Patch(0x1, 2)"', code)
        self.assertEqual(comments, {})

    def test_whole_line_comments_are_headings(self):
        _, _, comments = mi.clean("void f()\n{\n\t// Fix the thing\n\tx();\n}\n")
        self.assertEqual(comments, {3: "Fix the thing"})

    def test_digit_separator_and_char_literals(self):
        text = "int a = 1'000'000; char c = U'\\u00c0'; d = '{';\n{ }\n"
        _, masked, _ = mi.clean(text)
        self.assertEqual(masked.count("{"), 1)
        self.assertIn("1'000'000", masked)

    def test_backslash_newline_inside_string_keeps_lines(self):
        text = '#pragma x("a \\\nb")\nint y;\n'
        _, masked, _ = mi.clean(text)
        self.assertEqual([len(x) for x in masked.split("\n")], [len(x) for x in text.split("\n")])


class PreprocessTests(unittest.TestCase):
    def test_branches(self):
        text = "#if 0\nA();\n#elif defined(_GTA_SA) && !_GTA_III\nB();\n#else\nC();\n#endif\n#ifndef NDEBUG\nD();\n#endif\n"
        src = mi.load_source(text, ["_GTA_SA", "NDEBUG"])
        self.assertNotIn("A()", src.code)
        self.assertIn("B()", src.code)
        self.assertNotIn("C()", src.code)
        self.assertNotIn("D()", src.code)
        self.assertEqual(src.code.count("\n"), text.count("\n"))


class SilentPatchStyleTests(unittest.TestCase):
    TEXT = """
#include "x.h"
void Patch_SA_10(HINSTANCE hInstance)
{
\tusing namespace Memory;

\t// Fix the first thing
\tPatch<WORD>(0x4C9290, 0xE281);
\tPatch<int>(0x4C9292, ~1);
\t//Patch<BYTE>(0x5D7265, 0xEB);
\tNop(0x53E9A5, 1);

\t// Second fix
\t{
\t\tconst uintptr_t pAddress = AddressByRegion_10<uintptr_t>(0x748D9B);
\t\tReadCall( pAddress, RsEventHandler );
\t\tInjectHook(pAddress, NewFrameRender);
\t\tInjectHook(0x4C9223, TrailerFix, HookType::Jump);
\t\tPatch(0x4C7B76 + 1, &SetAtomicRendererCB);
\t\tPatch(0x4C7B80, { 0x90, 0x90, 0x90 });
\t}

\tif (bOption)
\t{
\t\tPatch<BYTE>(0x576CCC, 0xEB);
\t}
}

void Patch_SA_11()
{
\tPatch<BYTE>(0x576D0C, 0xEB);
}

void Patch_SA_NewBinaries_Common(HINSTANCE hInstance)
{
\tPatch<BYTE>(0x123456, 0xEB);
}

static ExternalMethod<CPed, void()> CPed::Thing(AddressByVersion<void (__thiscall*)(CPed*)>(0x5DF400, 0x5DFC20, 0x5FC120));
"""

    def setUp(self):
        self.rows, self.dropped = rows_for(SP_CFG, self.TEXT)
        self.by_addr = {r.addr: r for r in self.rows}

    def test_other_versions_dropped(self):
        self.assertEqual(self.dropped, {"1.01": 1, "newsteam": 1})
        self.assertNotIn(0x576D0C, self.by_addr)
        self.assertNotIn(0x123456, self.by_addr)

    def test_commented_out_patch_ignored(self):
        self.assertNotIn(0x5D7265, self.by_addr)

    def test_patch_sizes(self):
        self.assertEqual(self.by_addr[0x4C9290].span, 2)
        self.assertEqual(self.by_addr[0x4C9292].span, 4)
        self.assertEqual(self.by_addr[0x53E9A5].span, 1)
        self.assertEqual(self.by_addr[0x4C7B77].span, 4)
        self.assertEqual(self.by_addr[0x4C7B80].span, 3)

    def test_ops_and_pins(self):
        hooks = [r for r in self.rows if r.addr == 0x748D9B]
        self.assertEqual(sorted(r.op for r in hooks), ["inject_hook", "read_call"])
        self.assertTrue(all("region_10" in r.pin for r in hooks))
        self.assertEqual(self.by_addr[0x4C9223].op, "inject_jump")
        self.assertEqual(self.by_addr[0x4C9223].target, "TrailerFix")
        self.assertEqual(self.by_addr[0x4C7B77].pin, "literal")

    def test_groups_scope_and_condition(self):
        self.assertEqual(self.by_addr[0x4C9290].group, "Fix the first thing")
        self.assertEqual(self.by_addr[0x748D9B].group, "Second fix")
        self.assertEqual(self.by_addr[0x576CCC].cond, "bOption")
        self.assertEqual(self.by_addr[0x4C9290].scope, "Patch_SA_10")
        self.assertEqual(self.by_addr[0x4C9290].version, "1.0")

    def test_binding_takes_the_10_address(self):
        b = self.by_addr[0x5DF400]
        self.assertEqual(b.op, "bind")
        self.assertIn("version_10", b.pin)
        self.assertEqual(b.version, "any")
        self.assertEqual(b.group, "bindings: src.cpp")

    def test_ids_survive_unrelated_edits(self):
        moved, _ = rows_for(SP_CFG, "\n\n// unrelated\nint x;\n" + self.TEXT)
        self.assertEqual(sorted(r.id for r in moved), sorted(r.id for r in self.rows))
        self.assertEqual(len({r.id for r in self.rows}), len(self.rows))


class PatternTests(unittest.TestCase):
    def setUp(self):
        code = bytearray(0x100)
        code[0x10:0x16] = bytes([0x8B, 0x0D, 0x11, 0x22, 0x33, 0x44])
        code[0x40:0x45] = bytes([0xE8]) + struct.pack("<i", 0x80 - 0x45)
        code[0x45:0x48] = bytes([0x5E, 0xC3, 0xCC])
        code[0x60:0x63] = bytes([0x6A, 0x01, 0x90])
        code[0x70:0x73] = bytes([0x6A, 0x01, 0x90])
        self.image = fake_image(bytes(code))

    def test_pattern_forms(self):
        text = """
export void InitThing()
{
    FusionFix::onInitEvent() += []()
    {
        auto pattern = hook::pattern("8B 0D ? ? ? ?");
        injector::WriteMemory(pattern.get_first(2), &value, true);
        injector::MakeCALL(hook::get_pattern("E8 ? ? ? ? 5E C3", 0), Hook, true);
        static auto h = safetyhook::create_inline(injector::GetBranchDestination(hook::get_pattern("E8 ? ? ? ? 5E")).as_int(), Fn);
        pattern = hook::pattern("6A 01 90");
        for (size_t i = 0; i < pattern.size(); i++)
            injector::MakeNOP(pattern.get(i).get<void>(0), 3, true);
        injector::MakeNOP(hook::get_pattern("DE AD BE EF"), 2, true);
    };
}
"""
        rows, _ = rows_for(WS_CFG, text, self.image)
        got = {(r.op, r.addr) for r in rows}
        self.assertIn(("patch", CODE_VA + 0x12), got)
        self.assertIn(("make_call", CODE_VA + 0x40), got)
        self.assertIn(("inline_hook", CODE_VA + 0x80), got)
        self.assertIn(("nop", CODE_VA + 0x60), got)
        self.assertIn(("nop", CODE_VA + 0x70), got)
        missing = [r for r in rows if r.addr is None]
        self.assertEqual(len(missing), 1)
        self.assertEqual(missing[0].pin, "unresolved")
        self.assertEqual(missing[0].note, "pattern not found")
        for r in rows:
            self.assertNotIn("DE AD", r.expr)
            self.assertNotIn("8B 0D", r.expr)

    def test_reroute_follows_jmp_and_range_patterns(self):
        code = bytearray(0x400)
        code[0x100:0x105] = b"\xe9" + struct.pack("<i", 0x200 - 0x105)
        sig = bytes.fromhex("56e8000000008a962d040000")
        code[0x210:0x210 + len(sig)] = sig
        code[0x300:0x300 + len(sig)] = sig
        image = fake_image(bytes(code))
        text = """
void Patch_SA_10(HINSTANCE hInstance)
{
\t// Rerouted fix
\t{
\t\tuintptr_t f = ModCompat::Utils::GetFunctionAddrIfRerouted(0x401100);
\t\tauto site = get_pattern({{ f, f + 0x40 }}, "56 E8 ? ? ? ? 8A 96 2D 04 00 00", 1);
\t\tInterceptCall(site, org, Hook);
\t\tuintptr_t place = ModCompat::Utils::GetFunctionAddrIfRerouted(0x401100) + 0x5;
\t\tNop(place, 2);
\t\tNop(ModCompat::Utils::GetFunctionAddrIfRerouted(0x401300), 1);
\t}
}
"""
        rows, _ = rows_for(SP_CFG, text, image)
        got = {(r.op, r.addr) for r in rows}
        self.assertIn(("intercept_call", CODE_VA + 0x211), got)
        self.assertIn(("nop", CODE_VA + 0x205), got)
        self.assertIn(("nop", CODE_VA + 0x300), got)
        self.assertNotIn(("intercept_call", CODE_VA + 0x301), got)
        self.assertTrue(all(r.addr is not None for r in rows))

    def test_pattern_without_exe_is_unresolved(self):
        rows, _ = rows_for(WS_CFG, 'void f() { injector::MakeNOP(hook::get_pattern("8B 0D"), 2, true); }')
        self.assertEqual(rows[0].pin, "unresolved")
        self.assertEqual(rows[0].note, "pattern needs --exe")

    def test_range_for_and_helper_parameters(self):
        text = """
void Hook(uintptr_t address)
{
    injector::MakeCALL(address, Fn, true);
}
void Init()
{
    for (auto address : { 0x401010, 0x401020 })
        injector::WriteMemory(address + 2, &v, true);
    Hook(0x401030);
    Hook(0x401040);
}
"""
        rows, _ = rows_for(WS_CFG, text, self.image)
        got = sorted((r.op, r.addr) for r in rows)
        self.assertEqual(got, [("make_call", 0x401030), ("make_call", 0x401040), ("patch", 0x401012), ("patch", 0x401022)])

    def test_hook_each_lists_resets_and_table_reads(self):
        text = """
template<typename T>
static void AddVariableToReset(T** ptr)
{
}
void Patch_SA_10()
{
    // Hook these calls
    std::array<uintptr_t, 2> calls = { 0x401010, 0x401020 };
    Fix::HookEach_Calls(calls, InterceptCall);
    // Scale these sizes
    std::array<float**, 2> sizes = { (float**)(0x401030 + 2), (float**)(0x401040 + 2) };
    HookEach_Sizes(sizes, InterceptMemDisplacement);
    // Reset these
    AddVariableToReset( (bool**)(0x401050+1) );
    // Tables
    auto table = *reinterpret_cast<int16_t (**)[8]>(0x401060 + 1);
}
"""
        rows, _ = rows_for(SP_CFG, text, self.image)
        got = sorted((r.op, r.addr, r.group) for r in rows)
        self.assertEqual(got, [
            ("intercept_call", 0x401010, "Hook these calls"),
            ("intercept_call", 0x401020, "Hook these calls"),
            ("intercept_displacement", 0x401032, "Scale these sizes"),
            ("intercept_displacement", 0x401042, "Scale these sizes"),
            ("read", 0x401051, "Reset these"),
            ("read", 0x401061, "Tables"),
        ])


class TriageTests(unittest.TestCase):
    def rows(self):
        return [
            mi.Row("sp-1", "patch", 0x401000, 1, "literal", "1.0", "Fix A", "Patch_SA_10", "", "", "0xEB", "0x401000", "a.cpp", 3, ""),
            mi.Row("sp-2", "patch", 0x401001, 1, "literal", "1.0", "Fix A", "Patch_SA_10", "", "", "0xEB", "0x401001", "a.cpp", 4, ""),
            mi.Row("sp-3", "nop", None, 2, "unresolved", "1.0", "FLAC support", "Patch_SA_10", "", "", "", "x", "a.cpp", 9, "unresolved name x"),
        ]

    def test_inventory_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "inventory.yaml"
            mi.write_inventory(p, "silentpatch", SP_CFG, self.rows(), {"1.01": 4}, ["a.cpp"])
            data = mi.read_yaml(p.read_text())
        self.assertEqual(data["sites"], 3)
        self.assertEqual(data["dropped"], {"1.01": 4})
        self.assertEqual(data["rows"][0]["addr"], 0x401000)
        self.assertIsNone(data["rows"][2]["addr"])
        back = mi.rows_from_inventory(data)
        self.assertEqual(back[1].id, "sp-2")

    def test_triage_merge_keeps_decisions_and_hints(self):
        text, gone = mi.build_triage("silentpatch", SP_CFG, self.rows(), None, None)
        tri = mi.read_yaml(text)
        groups = {g["group"]: g for g in tri["groups"]}
        self.assertEqual(groups["Fix A"]["sites"], ["sp-1", "sp-2"])
        self.assertTrue(groups["FLAC support"]["hint"].startswith("drop:"))
        tri["groups"][0]["decision"] = "keep"
        tri["groups"][0]["reason"] = "game logic"
        tri["groups"].append({"group": "Old", "decision": "drop", "reason": "gone", "sites": []})
        text2, gone2 = mi.build_triage("silentpatch", SP_CFG, self.rows(), tri, None)
        groups2 = {g["group"]: g for g in mi.read_yaml(text2)["groups"]}
        self.assertEqual(groups2["Fix A"]["decision"], "keep")
        self.assertEqual(groups2["Fix A"]["reason"], "game logic")
        self.assertNotIn("hint", groups2["Fix A"])
        self.assertEqual(gone2, ["Old"])

    def test_pins_fill_and_expand_unresolved_rows(self):
        rows = self.rows()
        pins = {"pins": [{"id": "sp-3", "addrs": [0x500000, 0x500010], "reason": "two callers"},
                         {"id": "sp-1", "addr": 0x401100, "reason": "moved"},
                         {"id": "sp-9", "addr": 0x1, "reason": "unknown"}]}
        with redirect_stderr(io.StringIO()) as err:
            out = mi.apply_pins("silentpatch", rows, pins)
        self.assertIn("unknown site sp-9", err.getvalue())
        self.assertEqual([r.id for r in out], ["sp-1", "sp-2", "sp-3.0", "sp-3.1"])
        self.assertEqual([r.addr for r in out if r.id.startswith("sp-3")], [0x500000, 0x500010])
        self.assertTrue(all(r.pin == "manual" for r in out if r.id != "sp-2"))
        self.assertEqual(out[0].addr, 0x401100)

    def test_pins_can_move_a_site_to_another_group(self):
        rows = self.rows()
        out = mi.apply_pins("silentpatch", rows, {"pins": [
            {"id": "sp-2", "group": "Ported together", "reason": "one fix"},
            {"id": "sp-3", "addr": 0x500000, "group": "Ported together", "reason": "pinned and moved"}]})
        self.assertEqual([r.group for r in out], ["Fix A", "Ported together", "Ported together"])
        self.assertEqual(out[1].addr, 0x401001)
        self.assertEqual(out[1].pin, "literal")
        text, _ = mi.build_triage("silentpatch", SP_CFG, out, None, None)
        groups = {g["group"]: g["sites"] for g in mi.read_yaml(text)["groups"]}
        self.assertEqual(groups["Ported together"], ["sp-2", "sp-3"])

    def test_hints_leave_game_logic_alone(self):
        self.assertEqual(mi.hint_for("InjectDelayedPatches_10", [])[0], "")
        self.assertEqual(mi.hint_for("Fix credits not scaling to resolution", [])[0], "")
        self.assertEqual(mi.hint_for("Dialog text/window and radio scan progress must also scale", [])[0], "")
        self.assertEqual(mi.hint_for("Improved resolution selection dialog", [])[0], "replace")
        self.assertEqual(mi.hint_for("ALLOW ALT+TABBING WITHOUT PAUSING", [])[0], "replace")
        self.assertEqual(mi.hint_for("Disable re-initialization of DirectInput mouse device", [])[0], "replace")

    def test_check_mod_reports_problems(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "inventory.yaml"
            mi.write_inventory(p, "silentpatch", SP_CFG, self.rows(), {}, ["a.cpp"])
            inv = mi.read_yaml(p.read_text())
        tri = {"groups": [
            {"group": "Fix A", "decision": "keep", "reason": "", "sites": ["sp-1", "sp-9"]},
            {"group": "B", "decision": "maybe", "reason": "x", "sites": ["sp-1"]},
        ]}
        errors, stats = mi.check_mod("silentpatch", inv, tri)
        text = "\n".join(errors)
        self.assertIn("keep without a reason", text)
        self.assertIn("unknown site sp-9", text)
        self.assertIn("decision 'maybe'", text)
        self.assertIn("sp-1 is in groups", text)
        self.assertIn("sp-2 is not in any triage group", text)

    def test_cross_mod_overlap(self):
        rows = {
            "silentpatch": [{"id": "sp-1", "op": "patch", "addr": 0x401000, "span": 4}],
            "widescreen": [{"id": "ws-1", "op": "nop", "addr": 0x401002, "span": 2},
                           {"id": "ws-2", "op": "bind", "addr": 0x401000, "span": None}],
        }
        out = mi.overlaps(rows, {})
        self.assertEqual(len(out), 1)
        self.assertIn("sp-1", out[0])
        self.assertIn("ws-1", out[0])
        self.assertEqual(mi.overlaps(rows, {"widescreen": {"ws-1": "drop"}}), [])


class ManifestLinkTests(unittest.TestCase):
    def test_mod_entries_need_a_kept_site(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "silentpatch.yaml").write_text(
                "- addr: 0x00401000\n  kind: byte_patch\n  source: silentpatch\n  group: a\n  bytes: \"90\"\n\n"
                "- addr: 0x00401002  # comment\n  kind: nop\n  source: silentpatch\n  group: b\n  len: 1\n\n"
                "- addr: 0x00409000\n  kind: nop\n  source: silentpatch\n  group: c\n  len: 1\n\n"
                "- addr: 0x00401000\n  kind: func_replace\n  source: native\n  group: n\n  target: x\n")
            entries = mi.read_manifest_entries(Path(d))
        self.assertEqual(len(entries), 4)
        rows = {"silentpatch": [{"id": "sp-1", "addr": 0x401000, "span": 1}, {"id": "sp-2", "addr": 0x401001, "span": 2}]}
        errors = mi.manifest_errors(entries, rows, {"silentpatch": {"sp-1": "keep", "sp-2": "pending"}})
        self.assertEqual(len(errors), 2)
        self.assertIn("silentpatch.yaml:7", errors[0])
        self.assertIn("sp-2 pending", errors[0])
        self.assertIn("matches no silentpatch inventory site", errors[1])

    def test_entries_can_name_the_site_they_port(self):
        entries = [
            {"file": "w.yaml", "line": 1, "addr": 0x40100a, "kind": "mid_hook", "source": "widescreen", "site": "ws-1"},
            {"file": "w.yaml", "line": 9, "addr": 0x40100a, "kind": "mid_hook", "source": "widescreen", "site": "ws-9"},
            {"file": "w.yaml", "line": 17, "addr": 0x401000, "kind": "mid_hook", "source": "widescreen", "site": "ws-2"},
        ]
        rows = {"widescreen": [{"id": "ws-1", "addr": 0x401000, "span": 5}, {"id": "ws-2", "addr": 0x401000, "span": 5}]}
        errors = mi.manifest_errors(entries, rows, {"widescreen": {"ws-1": "keep", "ws-2": "pending"}})
        self.assertEqual(len(errors), 2)
        self.assertIn("site ws-9 is not in the widescreen inventory", errors[0])
        self.assertIn("ws-2 pending", errors[1])

    def test_address_lists_expand_into_entries(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "widescreen.yaml").write_text(
                "- addr: 0x00401000, 0x00401010,\n    0x00401020\n  kind: operand_slot\n  source: widescreen\n"
                "  group: hud\n  target: ws_hud_scale\n\n"
                "- addr: 0x00866b78\n  kind: data_slot\n  source: widescreen\n  group: hud\n  target: ws_radar_width\n")
            entries = mi.read_manifest_entries(Path(d))
        self.assertEqual([e["addr"] for e in entries], [0x401000, 0x401010, 0x401020, 0x866b78])
        self.assertEqual(entries[2]["target"], "ws_hud_scale")
        self.assertEqual(entries[2]["line"], 1)
        self.assertEqual([mi.manifest_span(e) for e in entries], [6, 6, 6, 4])
        rows = {"widescreen": [{"id": "ws-1", "addr": 0x401012, "span": 4}]}
        errors = mi.manifest_errors(entries, rows, {"widescreen": {"ws-1": "keep"}})
        self.assertEqual(len(errors), 3)
        self.assertIn("0x00401000 matches no widescreen inventory site", errors[0])


if __name__ == "__main__":
    unittest.main()
