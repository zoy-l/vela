"""Validate the generated extension package without requiring npm or VS Code."""

from __future__ import annotations

import json
from pathlib import Path
import re
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

from tools.editor import CONTENT_NAMESPACE, MANIFEST_NAMESPACE, package_extension


class EditorPackagingTest(unittest.TestCase):
    def test_numeric_highlighting_covers_complete_radix_literals(self) -> None:
        grammar_path = Path(__file__).resolve().parents[1] / "editors/vscode/syntaxes/vela.tmLanguage.json"
        grammar = json.loads(grammar_path.read_text(encoding="utf-8"))
        pattern = next(item["match"] for item in grammar["patterns"]
                       if item.get("name") == "constant.numeric.vela")
        for literal in ("0x2a", "0XFF", "0b101010", "0B01", "0o52", "0O07",
                        "42", "1.5e+2"):
            with self.subTest(literal=literal):
                self.assertIsNotNone(re.fullmatch(pattern, literal))
                self.assertEqual(re.findall(pattern, f"[{literal},"), [literal])
        for literal in ("0x", "0b102", "0o8", "0x1g", "0b1_0"):
            with self.subTest(literal=literal):
                self.assertIsNone(re.search(pattern, literal))

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
                self.assertEqual(next(item.attrib['ContentType'] for item in content
                                      if item.attrib.get('Extension') == 'svg'), 'image/svg+xml')
                for asset in manifest.findall(f".//{{{MANIFEST_NAMESPACE}}}Asset"):
                    self.assertIn(asset.attrib["Path"], archive.namelist())
                package = json.loads(archive.read("extension/package.json"))
                self.assertFalse(package.get("dependencies"))
                self.assertTrue(package['contributes']['configurationDefaults']['[vela]']['editor.semanticHighlighting.enabled'])
                scopes = package['contributes']['semanticTokenScopes'][0]
                self.assertEqual(scopes['language'], 'vela')
                self.assertIn('class', scopes['scopes'])
                self.assertIn(f'extension/{package["main"].removeprefix("./")}', archive.namelist())
                for grammar in package["contributes"]["grammars"]:
                    textmate = json.loads(archive.read(f'extension/{grammar["path"].removeprefix("./")}'))
                    self.assertEqual(grammar["scopeName"], textmate["scopeName"])
                for language in package["contributes"]["languages"]:
                    json.loads(archive.read(f'extension/{language["configuration"].removeprefix("./")}'))
                    for theme in ('light', 'dark'):
                        icon = language['icon'][theme].removeprefix('./')
                        svg = ET.fromstring(archive.read(f'extension/{icon}'))
                        self.assertEqual(svg.tag, '{http://www.w3.org/2000/svg}svg')
                        self.assertEqual(svg.attrib['viewBox'], '0 0 16 16')
                self.assertNotIn("extension/extension_unittest.cjs", archive.namelist())
            # Repack replaces an existing output with a complete valid archive.
            package_extension(destination)
            with zipfile.ZipFile(destination) as archive:
                self.assertIsNone(archive.testzip())


if __name__ == "__main__":
    unittest.main()
