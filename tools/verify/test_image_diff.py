import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import image_diff as idf


def solid(w, h, rgb):
    return idf.Image(w, h, bytearray(bytes(rgb) * (w * h)))


class ImageDiffTests(unittest.TestCase):
    def test_png_round_trip(self):
        img = solid(5, 3, (10, 20, 30))
        img.rgb[3:6] = bytes((200, 100, 50))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.png"
            idf.write_png(p, img)
            back = idf.read_png(p)
        self.assertEqual((back.width, back.height), (5, 3))
        self.assertEqual(back.pixel(1, 0), (200, 100, 50))
        self.assertEqual(back.pixel(4, 2), (10, 20, 30))

    def test_tolerance_ignore_and_bbox(self):
        a = solid(10, 10, (100, 100, 100))
        b = solid(10, 10, (104, 100, 100))
        r = idf.compare(a, b, 8)
        self.assertEqual(r.differing, 0)
        self.assertEqual(r.max_delta, 4)
        b.rgb[(2 * 10 + 3) * 3] = 200
        b.rgb[(5 * 10 + 6) * 3] = 0
        r = idf.compare(a, b, 8)
        self.assertEqual(r.differing, 2)
        self.assertEqual(r.bbox, (3, 2, 4, 4))
        r = idf.compare(a, b, 8, [(0, 0, 5, 5)])
        self.assertEqual(r.differing, 1)
        self.assertEqual(r.compared, 75)

    def test_cli_verdicts(self):
        with tempfile.TemporaryDirectory() as d:
            a, b, diff = Path(d) / "a.png", Path(d) / "b.png", Path(d) / "d.png"
            idf.write_png(a, solid(20, 20, (0, 0, 0)))
            other = solid(20, 20, (0, 0, 0))
            other.rgb[0:3] = bytes((255, 255, 255))
            idf.write_png(b, other)
            self.assertEqual(idf.main([str(a), str(b), "--max-fraction", "0.01", "--diff", str(diff)]), 0)
            self.assertEqual(idf.read_png(diff).pixel(0, 0), (255, 0, 255))
            self.assertEqual(idf.main([str(a), str(b), "--max-fraction", "0"]), 1)
            idf.write_png(b, solid(10, 20, (0, 0, 0)))
            self.assertEqual(idf.main([str(a), str(b)]), 2)


class RegionTests(unittest.TestCase):
    def test_regions_compare_different_places(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.png", Path(d) / "b.png"
            wide = solid(16, 4, (0, 0, 0))
            narrow = solid(8, 4, (0, 0, 0))
            for y in range(4):
                for x in range(8):
                    wide.rgb[(y * 16 + x + 4) * 3:(y * 16 + x + 4) * 3 + 3] = bytes((x * 30, y * 60, 7))
                    narrow.rgb[(y * 8 + x) * 3:(y * 8 + x) * 3 + 3] = bytes((x * 30, y * 60, 7))
            idf.write_png(a, wide)
            idf.write_png(b, narrow)
            self.assertEqual(idf.main([str(a), str(b), "--region", "4,0,8,4", "--reference-region", "0,0,8,4",
                                       "--max-fraction", "0"]), 0)
            self.assertEqual(idf.main([str(a), str(b), "--region", "0,0,8,4", "--reference-region", "0,0,8,4",
                                       "--max-fraction", "0"]), 1)
            self.assertEqual(idf.main([str(a), str(b), "--region", "12,0,8,4"]), 2)


if __name__ == "__main__":
    unittest.main()
