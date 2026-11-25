import importlib.util
from pathlib import Path
import sys
import unittest

SCRIPTS = Path(__file__).resolve().parents[2] / "lib/EpdFont/scripts"
sys.path.insert(0, str(SCRIPTS))
import fontconvert_sdcard as converter

class CombiningPresetTest(unittest.TestCase):
    def test_all_combining_marks_requested(self):
        for preset in ("latin-ext", "latin-ext,greek,cyrillic", "reading", "builtin"):
            intervals = converter.resolve_intervals(preset)
            for codepoint in range(0x0300, 0x0370):
                self.assertTrue(converter.code_point_in_intervals(codepoint, intervals), (preset, codepoint))

    def test_ascii_unchanged(self):
        self.assertFalse(converter.code_point_in_intervals(0x0301, converter.resolve_intervals("ascii")))

if __name__ == "__main__":
    unittest.main()
