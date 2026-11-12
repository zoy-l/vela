"""Check the unified LLVM installation without downloads or native tool execution."""

from __future__ import annotations

import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

from tools import clang_sdk, config, llvm


class ToolchainLayoutTest(unittest.TestCase):
    def test_supported_hosts_use_one_release_for_tools_headers_and_resources(self) -> None:
        cases = (
            ("Windows", "AMD64", "windows-amd64", "clang-cl.exe", "clang-cl.exe",
             "llvm-lib.exe", "lld-link.exe"),
            ("Linux", "x86_64", "linux-amd64", "clang", "clang++", "llvm-ar", "ld.lld"),
            ("Darwin", "arm64", "mac-arm64", "clang", "clang++", "llvm-ar", "ld64.lld"),
        )
        for system, machine, host, cc, cxx, archiver, linker in cases:
            with self.subTest(host=host), tempfile.TemporaryDirectory(prefix="vela-llvm-layout-") as work:
                root = Path(work)
                with (mock.patch.object(config.platform, "system", return_value=system),
                      mock.patch.object(config.platform, "machine", return_value=machine),
                      mock.patch.object(config, "BUILD_TOOLS_ROOT", root),
                      mock.patch.object(llvm, "BUILD_TOOLS_ROOT", root),
                      mock.patch.object(config, "xcode_path", return_value=root / "system-sdk")):
                    version, _, _, _ = llvm.sdk_specification()
                    directory = root / "llvm_toolchain" / version / host
                    suffix = ".exe" if system == "Windows" else ""
                    expected = {"cc": cc, "cxx": cxx, "archiver": archiver, "linker": linker,
                                "clang_format": "clang-format" + suffix,
                                "clangd": "clangd" + suffix, "clang_tidy": "clang-tidy" + suffix}
                    inputs = [directory / "bin" / name for name in expected.values()]
                    inputs += [directory / ("bin/llvm-config" + suffix),
                               directory / "include/llvm/IR/Module.h",
                               directory / "include/clang/Tooling/Tooling.h",
                               directory / f"lib/clang/{version.split('.')[0]}/include/stddef.h"]
                    inputs += [root / name / host / (name + suffix) for name in ("gn", "ninja")]
                    for path in inputs:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.touch()
                    llvm.validate_sdk(directory, version)
                    tools = config.toolchain_tools()
                    for name, filename in expected.items():
                        self.assertEqual(tools[name], directory / "bin" / filename)
                    self.assertEqual(llvm.sdk_directory(), directory)
                    self.assertEqual(clang_sdk.resource_directory(),
                                     directory / "lib/clang" / version.split(".")[0])
                    tools["clang_tidy"].unlink()
                    with self.assertRaisesRegex(RuntimeError, "clang-tidy"):
                        llvm.validate_sdk(directory, version)


class ArchiveExtractionTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="vela-llvm-archive-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.archive = self.root / "llvm.tar.xz"
        self.destination = self.root / "installed"
        self.destination.mkdir()

    def write_archive(self, files: dict[str, bytes],
                      links: tuple[tuple[str, str, bytes], ...] = ()) -> None:
        with tarfile.open(self.archive, "w:xz") as package:
            for name, target, kind in links:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.linkname = target
                package.addfile(member)
            for name, content in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(content)
                member.mode = 0o755
                package.addfile(member, io.BytesIO(content))

    def test_retains_tools_development_inputs_shared_libraries_and_forward_aliases(self) -> None:
        files = {"bin/clang-23": b"compiler", "bin/clang-tidy": b"lint",
                 "lib/libLLVM.so.23": b"shared", "bin/llvm-ar": b"archive",
                 "include/c++/v1/vector": b"libc++", "include/llvm/IR/Module.h": b"llvm",
                 "include/clang/Tooling/Tooling.h": b"clang",
                 "lib/clang/23/include/stddef.h": b"resource", "bin/runtime.dll": b"runtime",
                 "share/licenses/LLVM.txt": b"license"}
        self.write_archive({"release/" + name: data for name, data in files.items()}, (
            ("release/bin/clang++", "clang", tarfile.SYMTYPE),
            ("release/bin/clang", "clang-23", tarfile.SYMTYPE),
            ("release/lib/libLLVM.so", "libLLVM.so.23", tarfile.SYMTYPE),
            ("release/bin/llvm-ranlib", "release/bin/llvm-ar", tarfile.LNKTYPE),
        ))
        llvm.extract_sdk(self.archive, self.destination)
        for name, data in files.items():
            self.assertEqual((self.destination / name).read_bytes(), data)
        for name in ("clang", "clang++"):
            self.assertEqual((self.destination / "bin" / name).read_bytes(), b"compiler")
        self.assertEqual((self.destination / "bin/llvm-ranlib").read_bytes(), b"archive")
        self.assertEqual((self.destination / "lib/libLLVM.so").read_bytes(), b"shared")
        if os.name != "nt":
            self.assertTrue(os.access(self.destination / "bin/clang++", os.X_OK))

    @unittest.skipIf(os.name == "nt", "Unix release directory aliases require symbolic links")
    def test_retains_internal_directory_aliases(self) -> None:
        self.write_archive({"release/lib/runtime": b"shared"}, (
            ("release/lib64", "lib", tarfile.SYMTYPE),
        ))
        llvm.extract_sdk(self.archive, self.destination)
        self.assertEqual((self.destination / "lib64/runtime").read_bytes(), b"shared")

    def test_rejects_paths_and_aliases_outside_the_package(self) -> None:
        self.write_archive({"release/../outside": b"invalid"})
        with self.assertRaisesRegex(RuntimeError, "Invalid LLVM archive path"):
            llvm.extract_sdk(self.archive, self.destination)
        self.write_archive({}, (("release/bin/escape", "../../outside", tarfile.SYMTYPE),))
        with self.assertRaisesRegex(RuntimeError, "Invalid LLVM tool alias"):
            llvm.extract_sdk(self.archive, self.destination)
        self.assertFalse((self.root / "outside").exists())

    def test_rejects_missing_or_cyclic_alias_targets(self) -> None:
        for links in ((("release/bin/missing", "absent", tarfile.SYMTYPE),),
                      (("release/bin/first", "second", tarfile.SYMTYPE),
                       ("release/bin/second", "first", tarfile.SYMTYPE))):
            with self.subTest(links=links):
                self.write_archive({}, links)
                with self.assertRaisesRegex(RuntimeError, "Unresolved LLVM tool alias"):
                    llvm.extract_sdk(self.archive, self.destination)


if __name__ == "__main__":
    unittest.main()
