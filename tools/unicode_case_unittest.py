"""Verify pinned casing data and deterministic offline generation."""

from pathlib import Path
import json
import tempfile
import unittest

from tools import unicode_case


class UnicodeCaseDataTest(unittest.TestCase):
    def test_offline_generation_covers_full_mappings_and_context_properties(self) -> None:
        manifest = json.loads(unicode_case.MANIFEST.read_text(encoding="utf-8"))
        mappings, properties = unicode_case.tables(manifest)
        by_point = dict(mappings)
        self.assertEqual(by_point[0x00df][1], [0x53, 0x53])
        self.assertEqual(by_point[0x0130][0], [0x69, 0x0307])
        self.assertGreater(len(mappings), 100)
        for ranges in properties.values():
            self.assertTrue(all(first <= last for first, last in ranges))
            self.assertTrue(all(left[1] < right[0] - 1 for left, right in zip(ranges, ranges[1:])))
        first = unicode_case.generate(manifest)
        second = unicode_case.generate(manifest)
        self.assertEqual(first, second)
        self.assertIn(f'"{manifest["unicode_version"]}"', first)

    def test_input_identity_and_utf8proc_version_must_match(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-ucd-") as directory:
            sample = Path(directory) / "input.txt"
            sample.write_bytes(b"changed data")
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                unicode_case.checked_bytes(sample, "0" * 64)
        manifest = json.loads(unicode_case.MANIFEST.read_text(encoding="utf-8"))
        manifest["unicode_version"] = "0.0.0"
        with self.assertRaisesRegex(ValueError, "must match"):
            unicode_case.tables(manifest)


if __name__ == "__main__":
    unittest.main()
