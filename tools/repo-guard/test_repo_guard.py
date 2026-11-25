import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import repo_guard


def fake_pe() -> bytes:
    data = bytearray(0x80)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x40)
    data[0x40:0x44] = b"PE\0\0"
    return bytes(data)


def reasons(rel: str, data: bytes, allow=None):
    return [v.reason for v in repo_guard.check_file(rel, data, allow or set())]


class BinaryTests(unittest.TestCase):
    def test_pe_rejected_even_when_renamed(self):
        self.assertIn("Windows PE executable", reasons("docs/readme.txt", fake_pe()))

    def test_mz_text_without_pe_signature_allowed(self):
        self.assertEqual(reasons("notes.txt", b"MZ is a header, not a file" + b" " * 100), [])

    def test_bitcode_rejected(self):
        self.assertIn("LLVM bitcode", reasons("x.dat", b"BC\xc0\xde\x00\x00\x00\x00"))
        self.assertIn("LLVM bitcode", reasons("x.dat", b"\xde\xc0\x17\x0b\x00\x00\x00\x00"))

    def test_textual_ir_rejected(self):
        ir = b'target triple = "arm64-apple-macosx26.0.0"\n\ndefine void @sub_401000() {\n  ret void\n}\n'
        self.assertIn("LLVM textual IR", reasons("lifted.txt", ir))

    def test_macho_and_archive_rejected(self):
        self.assertIn("Mach-O object", reasons("a", b"\xcf\xfa\xed\xfe" + b"\0" * 12))
        self.assertIn("static archive", reasons("b", b"!<arch>\nfoo"))

    def test_denied_extension(self):
        self.assertIn("denied file type .ll", reasons("out/fn.ll", b"; empty"))
        self.assertIn("denied file type .img", reasons("models/gta3.IMG", b""))

    def test_game_directory(self):
        self.assertIn("game directory content", reasons("gamefiles/readme.txt", b"hi"))

    def test_size_limit_and_allowlist(self):
        big = b"a" * (repo_guard.MAX_FILE_BYTES + 1)
        self.assertTrue(any("limit" in r for r in reasons("big.txt", big)))
        self.assertEqual(reasons("big.txt", big, {"big.txt"}), [])

    def test_source_code_allowed(self):
        src = b"#include <cstdint>\nint main() { return 0; }\n"
        self.assertEqual(reasons("runtime/loader/src/x.cpp", src), [])


class AnalysisTests(unittest.TestCase):
    def test_addresses_allowed(self):
        row = b'{"entry":"0x00401000","section":".text","ranges":[["0x00401000","0x0040100f"]],"cc":"__stdcall","purge":8}\n'
        self.assertEqual(reasons("analysis/sa-1.0us/functions.json", row), [])

    def test_exe_sha256_field_allowed(self):
        line = b'  "exe_sha256": "a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26",\n'
        self.assertEqual(reasons("analysis/sa-1.0us/meta.json", line), [])

    def test_hex_blob_rejected(self):
        line = b'{"entry":"0x00401000","x":"8b4424048b4c240803c1c204008b442404"}\n'
        self.assertTrue(any("hex run" in r for r in reasons("analysis/sa-1.0us/functions.json", line)))

    def test_spaced_bytes_rejected(self):
        line = b"# 8B 44 24 04 8B 4C 24 08 03 C1 C2 04 00 90 90 90 90 90\n"
        self.assertTrue(any("byte sequence" in r for r in reasons("analysis/sa-1.0us/overrides.yaml", line)))

    def test_forbidden_keys_rejected(self):
        line = b'{"entry":"0x00401000","mnemonic":"mov"}\n'
        self.assertTrue(any("forbidden key" in r for r in reasons("analysis/sa-1.0us/functions.json", line)))

    def test_base64_rejected(self):
        line = b'"blob": "' + b"QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ejAxMjM0NTY3ODk" + b'"\n'
        self.assertTrue(any("base64" in r for r in reasons("analysis/sa-1.0us/x.json", line)))

    def test_hex_outside_analysis_allowed(self):
        line = b"constexpr auto kHash = \"a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26\";\n"
        self.assertEqual(reasons("runtime/loader/src/exe_identity.cpp", line), [])


class PathModeTests(unittest.TestCase):
    def test_paths_inside_repo_are_repo_relative(self):
        root = Path(repo_guard.__file__).resolve().parents[2]
        analysis = root / "analysis"
        if not analysis.is_dir():
            self.skipTest("no analysis directory")
        names = [rel for rel, _ in repo_guard.tree_files(root, [analysis])]
        self.assertTrue(names)
        self.assertTrue(all(name.startswith("analysis/") for name in names), names[:3])


if __name__ == "__main__":
    unittest.main()
