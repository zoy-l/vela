"""Describe LibTooling inputs from the project's pinned LLVM toolchain."""

from __future__ import annotations

from pathlib import Path
import sys

from tools.llvm import sdk_directory, sdk_specification


def resource_directory() -> Path:
    major_version = sdk_specification()[0].split(".")[0]
    return sdk_directory() / "lib/clang" / major_version


def link_configuration() -> dict[str, str | list[str]]:
    directory = sdk_directory()
    if not (directory / "include/clang/Tooling/Tooling.h").is_file():
        raise RuntimeError(f"Missing Clang SDK: {directory}; run python -m tools.vl llvm sync")
    # 固定 SDK 的 clang/lib 各 CMakeLists.txt 定义此静态依赖闭包。
    components = ("Tooling", "Frontend", "Serialization", "Driver", "DependencyScanning",
                  "ScalableStaticAnalysisFrontend", "ScalableStaticAnalysisAnalyses",
                  "ScalableStaticAnalysisSourceTransformation", "ScalableStaticAnalysisCore",
                  "UnifiedSymbolResolution", "Parse", "Sema", "AnalysisLifetimeSafety",
                  "Analysis", "ASTMatchers", "Format", "ToolingInclusions", "ToolingCore",
                  "Rewrite", "Edit", "APINotes", "AST", "Lex", "Options", "Basic", "Support")
    libraries = [directory / "lib" / (f"clang{name}.lib" if sys.platform == "win32"
                                      else f"libclang{name}.a") for name in components]
    for library in libraries:
        if not library.is_file():
            raise RuntimeError(f"Missing Clang static library: {library}")
    return {"vela_clang_root": directory.as_posix(),
            "vela_clang_resource_dir": resource_directory().as_posix(),
            "vela_clang_libraries": [path.as_posix() for path in libraries]}
