"""Validate audited target support before a native distribution is published."""

from __future__ import annotations

import json
from pathlib import Path
import stat
import tempfile
import unittest
from contextlib import ExitStack
from unittest.mock import patch

from tools.config import executable_name
from tools.package import copy_file, create_package, load_support_specification, verify_package
from tools.test_support import CommandTestCase, NATIVE_PACKAGE_AVAILABLE, create_native_package


class SupportSpecificationTest(unittest.TestCase):
    def test_packaged_files_are_writable_without_changing_cached_sources(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-support-") as temporary:
            root = Path(temporary)
            source = root / "cached-tool"
            source.write_bytes(b"tool")
            mode = source.stat().st_mode
            source.chmod(mode & ~stat.S_IWUSR)
            try:
                package = root / "package"
                copy_file(source, package, "bin/tool", "bindgen", "LLVM", "license")
                installed = package / "bin/tool"
                self.assertEqual(installed.read_bytes(), b"tool")
                self.assertTrue(installed.stat().st_mode & stat.S_IWUSR)
                self.assertFalse(source.stat().st_mode & stat.S_IWUSR)
                self.assertEqual(installed.stat().st_mode & stat.S_IXUSR,
                                 source.stat().st_mode & stat.S_IXUSR)
                installed.unlink()
            finally:
                source.chmod(mode)

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

    def test_existing_package_destinations_are_preserved(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-support-") as temporary:
            root = Path(temporary)
            for kind in ("file", "directory"):
                destination = root / kind
                if kind == "directory":
                    destination.mkdir()
                    original = destination / "original"
                else:
                    original = destination
                original.write_bytes(b"existing package")
                with self.subTest(kind=kind), self.assertRaisesRegex(
                        RuntimeError, "destination already exists"):
                    create_package(root / "compiler", root / "runtime", destination)
                self.assertEqual(original.read_bytes(), b"existing package")


@unittest.skipUnless(NATIVE_PACKAGE_AVAILABLE,
                     "This platform requires an audited VELA_TEST_TARGET_SUPPORT_SPEC")
class NativePackageCreationTest(CommandTestCase):
    directory_prefix = "vela-package-接口 "

    def test_verified_tools_can_remain_open_after_package_creation(self) -> None:
        destination = self.directory / "package"
        opened = []
        with ExitStack() as handles:
            def verify_and_open_tools(package: Path, workspace: Path,
                                      environment: dict[str, str]) -> None:
                verify_package(package, workspace, environment)
                target = json.loads((package / "share/vela/target.json").read_text(encoding="utf-8"))
                for relative in ("bin/" + executable_name("vela"), target["linker"]):
                    opened.append(handles.enter_context((package / relative).open("rb")))

            with patch("tools.package.verify_package", side_effect=verify_and_open_tools):
                create_native_package(self.executable, destination)
            self.assertEqual(len(opened), 2)
            for tool in opened:
                self.assertFalse(tool.closed)
                self.assertTrue(Path(tool.name).is_relative_to(destination))
                self.assertTrue(tool.read(1))
            self.assertEqual(list(self.directory.iterdir()), [destination])

    def test_failed_self_check_removes_package_and_allows_a_new_attempt(self) -> None:
        destination = self.directory / "package"

        def verify_with_invalid_abi(package: Path, workspace: Path,
                                    environment: dict[str, str]) -> None:
            target_path = package / "share/vela/target.json"
            target = json.loads(target_path.read_text(encoding="utf-8"))
            target["runtime_abi"] = "invalid"
            target_path.write_text(json.dumps(target), encoding="utf-8", newline="\n")
            verify_package(package, workspace, environment)

        with patch("tools.package.verify_package", side_effect=verify_with_invalid_abi):
            with self.assertRaisesRegex(RuntimeError, "Native package self-check failed"):
                create_native_package(self.executable, destination)
        self.assertEqual(list(self.directory.iterdir()), [])
        create_native_package(self.executable, destination)
        self.assertTrue((destination / "share/vela/target.json").is_file())

    def test_failed_copy_removes_incomplete_package(self) -> None:
        destination = self.directory / "package"

        def fail_runtime_copy(source: Path, package: Path, relative: str, role: str,
                              owner: str, license_name: str) -> dict:
            if role == "runtime":
                raise OSError("runtime copy failed")
            return copy_file(source, package, relative, role, owner, license_name)

        with patch("tools.package.copy_file", side_effect=fail_runtime_copy):
            with self.assertRaisesRegex(OSError, "runtime copy failed"):
                create_native_package(self.executable, destination)
        self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
