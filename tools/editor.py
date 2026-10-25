"""Package or test the dependency-free Vela VS Code extension."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile

from tools.config import ROOT


EDITOR = ROOT / "editors" / "vscode"
MANIFEST_NAMESPACE = "http://schemas.microsoft.com/developer/vsx-schema/2011"
CONTENT_NAMESPACE = "http://schemas.openxmlformats.org/package/2006/content-types"


def vsix_manifest(package: dict) -> bytes:
    ET.register_namespace("", MANIFEST_NAMESPACE)
    root = ET.Element(f"{{{MANIFEST_NAMESPACE}}}PackageManifest", {"Version": "2.0.0"})
    metadata = ET.SubElement(root, "Metadata")
    ET.SubElement(metadata, "Identity", {"Language": "en-US", "Id": package["name"],
                  "Version": package["version"], "Publisher": package["publisher"]})
    ET.SubElement(metadata, "DisplayName").text = package["displayName"]
    ET.SubElement(metadata, "Description").text = package["description"]
    ET.SubElement(metadata, "Categories").text = ",".join(package["categories"])
    properties = ET.SubElement(metadata, "Properties")
    ET.SubElement(properties, "Property", {"Id": "Microsoft.VisualStudio.Code.Engine", "Value": package["engines"]["vscode"]})
    ET.SubElement(properties, "Property", {"Id": "Microsoft.VisualStudio.Code.ExtensionKind", "Value": "workspace"})
    installation = ET.SubElement(root, "Installation")
    ET.SubElement(installation, "InstallationTarget", {"Id": "Microsoft.VisualStudio.Code"})
    ET.SubElement(root, "Dependencies")
    assets = ET.SubElement(root, "Assets")
    for kind, name in (("Microsoft.VisualStudio.Code.Manifest", "package.json"),
                       ("Microsoft.VisualStudio.Services.Content.Details", "README.md"),
                       ("Microsoft.VisualStudio.Services.Content.License", "LICENSE")):
        ET.SubElement(assets, "Asset", {"Type": kind, "Path": f"extension/{name}", "Addressable": "true"})
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def content_types() -> bytes:
    ET.register_namespace("", CONTENT_NAMESPACE)
    root = ET.Element(f"{{{CONTENT_NAMESPACE}}}Types")
    for extension, mime in (("json", "application/json"), ("js", "application/javascript"),
                            ("svg", "image/svg+xml"),
                            ("md", "text/markdown"), ("vsixmanifest", "text/xml")):
        ET.SubElement(root, "Default", {"Extension": extension, "ContentType": mime})
    ET.SubElement(root, "Override", {"PartName": "/extension/LICENSE", "ContentType": "text/plain"})
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def package_extension(destination: Path) -> Path:
    package = json.loads((EDITOR / "package.json").read_text(encoding="utf-8"))
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="vela-extension-", suffix=".tmp", dir=destination.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("extension.vsixmanifest", vsix_manifest(package))
            archive.writestr("[Content_Types].xml", content_types())
            for name in ("package.json", "extension.js", "language-configuration.json", "README.md",
                         "syntaxes/vela.tmLanguage.json", "icons/vela-light.svg", "icons/vela-dark.svg"):
                archive.write(EDITOR / name, f"extension/{name}")
            archive.write(ROOT / "LICENSE", "extension/LICENSE")
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def test_extension(code: Path) -> int:
    profile = Path(tempfile.mkdtemp(prefix="vela-vscode-profile-"))
    report = profile / "result.json"
    environment = os.environ.copy()
    environment["VELA_EDITOR_TEST_REPORT"] = str(report)
    try:
        result = subprocess.run([
            str(code.resolve()),
            f"--extensionDevelopmentPath={EDITOR}",
            f"--extensionTestsPath={EDITOR / 'editor_unittest.cjs'}",
            f"--user-data-dir={profile / 'user'}",
            f"--extensions-dir={profile / 'extensions'}",
            "--disable-extensions", "--skip-welcome", "--skip-release-notes",
            "--disable-workspace-trust", "--wait", str(ROOT),
        ], cwd=ROOT, env=environment, check=False)
        if not report.is_file():
            raise RuntimeError("VS Code exited without running the extension-host tests")
        outcome = json.loads(report.read_text(encoding="utf-8"))
        print(json.dumps(outcome, ensure_ascii=False, indent=2))
        return result.returncode or (0 if outcome["passed"] else 1)
    finally:
        # Windows 上扩展测试退出通知早于 Electron 子进程释放字典映射的时刻。
        for attempt in range(30):
            try:
                shutil.rmtree(profile)
                break
            except PermissionError:
                if attempt == 29:
                    raise
                time.sleep(0.1)


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["package", "test"])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--code", type=Path, help="VS Code launcher for real extension-host tests")
    parsed = parser.parse_args(arguments)
    if parsed.command == "test":
        if parsed.code is None or not parsed.code.is_file():
            parser.error("editor test requires --code PATH to a VS Code launcher")
        return test_extension(parsed.code)
    package = json.loads((EDITOR / "package.json").read_text(encoding="utf-8"))
    destination = parsed.output or ROOT / "out" / "editor" / f'{package["name"]}-{package["version"]}.vsix'
    print(package_extension(destination.resolve()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
