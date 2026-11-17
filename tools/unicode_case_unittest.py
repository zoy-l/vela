"""Verify pinned casing data and deterministic offline generation."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest

from tools import unicode_case


class UnicodeCaseDataTest(unittest.TestCase):
    def test_checkout_preserves_pinned_license_with_autocrlf_enabled(self) -> None:
        manifest = json.loads(unicode_case.MANIFEST.read_text(encoding="utf-8"))
        relative = Path("third_party/unicode_data/LICENSE")
        original = (unicode_case.ROOT / relative).read_bytes()
        with tempfile.TemporaryDirectory(prefix="vela-ucd-checkout-") as temporary:
            root = Path(temporary)
            license_file = root / relative
            license_file.parent.mkdir(parents=True)
            license_file.write_bytes(original)
            (root / ".gitattributes").write_bytes(
                (unicode_case.ROOT / ".gitattributes").read_bytes())
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            arguments = ["git", "-C", str(root), "-c", "core.autocrlf=true"]
            subprocess.run([*arguments, "add", ".gitattributes", relative.as_posix()],
                           check=True, capture_output=True)
            license_file.unlink()
            subprocess.run([*arguments, "checkout-index", "--all", "--force"],
                           check=True, capture_output=True)
            data = unicode_case.checked_bytes(license_file, manifest["license_sha256"])
            self.assertEqual(data, original)

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
