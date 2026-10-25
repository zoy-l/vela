"""Package the dependency-free Vela VS Code extension."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile
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
            for name in ("package.json", "extension.js", "language-configuration.json", "README.md", "syntaxes/vela.tmLanguage.json"):
                archive.write(EDITOR / name, f"extension/{name}")
            archive.write(ROOT / "LICENSE", "extension/LICENSE")
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["package"])
    parser.add_argument("--output", type=Path)
    parsed = parser.parse_args(arguments)
    package = json.loads((EDITOR / "package.json").read_text(encoding="utf-8"))
    destination = parsed.output or ROOT / "out" / "editor" / f'{package["name"]}-{package["version"]}.vsix'
    print(package_extension(destination.resolve()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
