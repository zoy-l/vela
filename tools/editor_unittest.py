"""Validate the generated extension package without requiring npm or VS Code."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

from tools.editor import CONTENT_NAMESPACE, MANIFEST_NAMESPACE, package_extension


class EditorPackagingTest(unittest.TestCase):
    def test_package_has_valid_manifest_assets_and_language_contributions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-vsix-") as temporary:
            destination = Path(temporary) / "vela.vsix"
            self.assertEqual(package_extension(destination), destination)
            with zipfile.ZipFile(destination) as archive:
                self.assertIsNone(archive.testzip())
                manifest = ET.fromstring(archive.read("extension.vsixmanifest"))
                content = ET.fromstring(archive.read("[Content_Types].xml"))
                self.assertEqual(manifest.tag, f"{{{MANIFEST_NAMESPACE}}}PackageManifest")
                self.assertEqual(content.tag, f"{{{CONTENT_NAMESPACE}}}Types")
                for asset in manifest.findall(f".//{{{MANIFEST_NAMESPACE}}}Asset"):
                    self.assertIn(asset.attrib["Path"], archive.namelist())
                package = json.loads(archive.read("extension/package.json"))
                self.assertFalse(package.get("dependencies"))
                self.assertIn(f'extension/{package["main"].removeprefix("./")}', archive.namelist())
                for grammar in package["contributes"]["grammars"]:
                    textmate = json.loads(archive.read(f'extension/{grammar["path"].removeprefix("./")}'))
                    self.assertEqual(grammar["scopeName"], textmate["scopeName"])
                for language in package["contributes"]["languages"]:
                    json.loads(archive.read(f'extension/{language["configuration"].removeprefix("./")}'))
                self.assertNotIn("extension/extension_unittest.cjs", archive.namelist())
            # Repack replaces an existing output with a complete valid archive.
            package_extension(destination)
            with zipfile.ZipFile(destination) as archive:
                self.assertIsNone(archive.testzip())


if __name__ == "__main__":
    unittest.main()
