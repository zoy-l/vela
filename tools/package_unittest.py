"""Validate audited target support before a native distribution is published."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.package import copy_file, load_support_specification


class SupportSpecificationTest(unittest.TestCase):
    def test_requires_target_and_auditable_sdk_and_redistribution_inputs(self) -> None:
        support = {
            "target": "x86_64-unknown-linux-gnu", "minimum_os": "Linux; audited libc baseline",
            "object_format": "ELF", "runtime_variant": "libc++; exact build identity",
            "files": [], "external_roots": [], "application_dependencies": ["libc.so.6"],
            "archive_arguments": ["rcs", "{output}", "{object}"],
            "link_arguments": ["-o", "{output}", "{object}", "{runtime}"],
            "bundled_files": [{"source_path": "crt1.o", "destination": "lib/crt1.o",
                               "source": "audited sysroot", "license": "sysroot license"}],
        }
        with tempfile.TemporaryDirectory(prefix="vela-support-") as temporary:
            path = Path(temporary) / "support.json"
            path.write_text(json.dumps(support), encoding="utf-8")
            self.assertEqual(load_support_specification(path, support["target"]), support)
            with self.assertRaisesRegex(RuntimeError, "target"):
                load_support_specification(path, "aarch64-apple-darwin")
            del support["bundled_files"][0]["license"]
            path.write_text(json.dumps(support), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "license"):
                load_support_specification(path, support["target"])

    def test_rejects_destinations_outside_package_without_writing_them(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-support-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.write_bytes(b"original")
            package = root / "package"
            package.mkdir()
            for destination in ("../source", str(source), "."):
                with self.subTest(destination=destination), self.assertRaisesRegex(
                        RuntimeError, "package-relative"):
                    copy_file(source, package, destination, "link", "sysroot", "license")
            self.assertEqual(source.read_bytes(), b"original")
            self.assertEqual(list(package.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
