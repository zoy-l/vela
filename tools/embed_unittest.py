from __future__ import annotations

from pathlib import Path
import unittest

from tools import embed


class EmbeddingContractTest(unittest.TestCase):
    def test_link_contract_preserves_system_dependencies_from_the_real_closure(self) -> None:
        targets = {
            "//compiler:vela_development": {"deps": ["//compiler:development"]},
            "//compiler:development": {"public_deps": ["//base:watcher", "//runtime:runtime"]},
            "//base:watcher": {"libs": ["pthread"], "frameworks": ["CoreServices.framework"]},
            "//runtime:runtime": {"libs": ["m"], "frameworks": ["CoreFoundation.framework"]},
            "//unrelated:tests": {"libs": ["test-only"]},
        }
        libraries, frameworks = embed.link_contract(targets, "//compiler:vela_development")
        self.assertCountEqual(libraries, ["m", "pthread"])
        self.assertCountEqual(frameworks, ["CoreFoundation.framework", "CoreServices.framework"])

    def test_split_libraries_resolves_gn_paths_without_turning_system_names_into_files(self) -> None:
        external = embed.ROOT / "sdk/libLLVM.a"
        archives, system = embed.split_libraries(
            ["//third_party/prebuilt.a", external.as_posix(), "m"])
        self.assertEqual(archives, [embed.ROOT / "third_party/prebuilt.a", external])
        self.assertEqual(system, ["m"])


if __name__ == "__main__":
    unittest.main()
