"""Keep the assistant reference synchronized with the language and SDK contracts."""

import unittest

from tools.config import ROOT
from tools.llms import render_reference


class ReferenceTest(unittest.TestCase):
    def test_generated_reference_matches_canonical_sources(self) -> None:
        self.assertEqual((ROOT / "llms-full.txt").read_text(encoding="utf-8"), render_reference())


if __name__ == "__main__":
    unittest.main()
