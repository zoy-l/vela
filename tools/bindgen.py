"""Generate and verify a complete native class binding generation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FORMAT_VERSION = 1
ROOT = Path(__file__).resolve().parents[1]


def canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def read_object(path: Path) -> dict:
    result = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(result, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return result


def cpp_sources(module: str, shards: int) -> list[str]:
    return [f"{module}_bindings.cc", *(f"{module}_bindings_{index}.cc"
                                      for index in range(1, shards))]


def verify_manifest(path: Path, visiting: set[Path] | None = None) -> dict:
    """Reject stale inputs and mixed products before any consumer uses a dependency."""
    path = path.resolve(strict=True)
    visiting = set() if visiting is None else visiting
    if path in visiting:
        raise ValueError(f"Cyclic binding dependency: {path}")
    visiting.add(path)
    manifest = read_object(path)
    if manifest.get("format_version") != FORMAT_VERSION:
        raise ValueError(f"Unsupported binding manifest: {path}")
    generation = manifest.get("generation")
    payload = {key: value for key, value in manifest.items() if key != "generation"}
    if generation != hashlib.sha256(canonical(payload)).hexdigest():
        raise ValueError(f"Invalid binding generation digest: {path}")
    bindings = manifest.get("bindings", {})
    if not isinstance(bindings, dict) or not isinstance(bindings.get("module"), str):
        raise ValueError(f"Invalid binding model: {path}")
    module = bindings.get("module")
    products = manifest.get("products", {})
    if not isinstance(products, dict) or len(products) < 4:
        raise ValueError(f"Incomplete binding products: {path}")
    expected = {f"{module}.vela", f"{module}_bindings.h", f"{module}.d",
                *cpp_sources(module, len(products) - 3)}
    if set(products) != expected:
        raise ValueError(f"Incomplete binding products: {path}")
    for name, expected_hash in products.items():
        product = path.parent / name
        if not product.is_file() or digest(product) != expected_hash:
            raise ValueError(f"Stale or mixed binding product: {product}")
    inputs = manifest.get("inputs")
    dependencies = manifest.get("dependencies")
    if not isinstance(inputs, dict) or not inputs or not isinstance(dependencies, list):
        raise ValueError(f"Incomplete binding inputs: {path}")
    for name, expected_hash in inputs.items():
        source = Path(name)
        if not source.is_file() or digest(source) != expected_hash:
            raise ValueError(f"Stale binding input: {source}")
    for dependency in dependencies:
        dependency_manifest = verify_manifest(Path(dependency), visiting)
        if dependency_manifest["bindings"]["target"] != bindings.get("target"):
            raise ValueError(f"Binding dependency target mismatch: {dependency}")
    visiting.remove(path)
    return manifest


def configuration(path: Path) -> dict:
    config = read_object(path)
    required = {"module", "headers", "translation_unit", "compilation_database", "output"}
    if not required <= config.keys() or config.keys() - required - {
            "dependencies", "callbacks", "opaque", "cpp_shards"}:
        raise ValueError("Binding config requires module, headers, translation_unit, "
                         "compilation_database, and output")
    shards = config.setdefault("cpp_shards", 1)
    if type(shards) is not int or not 1 <= shards <= 0xFFFFFFFF:
        raise ValueError("Binding config requires cpp_shards to be a positive unsigned integer")
    for name in required - {"headers"}:
        if not isinstance(config[name], str) or not config[name]:
            raise ValueError(f"Binding config requires a nonempty {name} string")
    for name in ("headers", "dependencies"):
        values = config.setdefault(name, [])
        if not isinstance(values, list) or any(not isinstance(value, str) or not value
                                               for value in values):
            raise ValueError(f"Binding config requires {name} to be a list of paths")
        if name == "headers" and not values:
            raise ValueError("Binding config requires at least one header")
        config[name] = sorted({(path.parent / value).resolve(strict=True) for value in values})
    for name in ("translation_unit", "compilation_database", "output"):
        config[name] = (path.parent / config[name]).resolve(strict=name != "output")
    for name in ("callbacks", "opaque"):
        values = config.setdefault(name, [])
        if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
            raise ValueError(f"Binding config requires {name} to be a list of C++ type names")
    return config


def depfile_escape(path: Path) -> str:
    return path.as_posix().replace("$", "$$").replace(" ", "\\ ").replace("#", "\\#").replace(":", "\\:")


def format_cpp(text: str, formatter: Path, style: Path) -> bytes:
    result = subprocess.run([str(formatter), "--style=file:" + str(style)],
                            input=text, capture_output=True, text=True, encoding="utf-8", check=True)
    return result.stdout.encode("utf-8")


def fingerprints(generator: Path, paths: list[Path | str]) -> dict[str, str]:
    result = subprocess.run([str(generator), "fingerprint"],
                            input=json.dumps([str(path) for path in paths]),
                            capture_output=True, text=True, encoding="utf-8", check=True)
    return json.loads(result.stdout)


def generate(config_path: Path, generator: Path, resource: Path, *, check: bool = False,
             formatter: Path | None = None, style: Path | None = None) -> Path:
    if formatter is None:
        from tools.config import toolchain_tools
        formatter = toolchain_tools()["clang_format"]
    formatter = formatter.resolve(strict=True)
    style = (style or ROOT / ".clang-format").resolve(strict=True)
    config_path = config_path.resolve(strict=True)
    config = configuration(config_path)
    dependency_paths = config["dependencies"]
    dependencies = [verify_manifest(path) for path in dependency_paths]
    module = config["module"]
    shards = config["cpp_shards"]
    sources = cpp_sources(module, shards)
    output = config["output"]
    manifest_path = output / f"{module}.bindings.json"
    generator = generator.resolve(strict=True)
    arguments = [str(generator), "--module=" + module,
                 "--database=" + str(config["compilation_database"]),
                 "--source=" + str(config["translation_unit"]),
                 "--resource-dir=" + str(resource.resolve(strict=True)),
                 "--output-directory=" + str(output),
                 f"--cpp-shards={shards}",
                 *["--header=" + str(path) for path in config["headers"]],
                 *["--callback=" + name for name in config["callbacks"]],
                 *["--opaque=" + name for name in config["opaque"]],
                 *["--dependency=" + str(path) for path in dependency_paths]]
    result = subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8", check=False)
    if result.returncode:
        raise ValueError(result.stderr.strip() or "Binding collection failed")
    bindings = json.loads(result.stdout)
    products = bindings.pop("products")
    if set(products) != {f"{module}.vela", f"{module}_bindings.h", *sources}:
        raise ValueError("Binding generator returned an unexpected set of products")
    parsed_fingerprints = bindings["input_fingerprints"]
    current_fingerprints = fingerprints(generator, list(parsed_fingerprints))
    if current_fingerprints != parsed_fingerprints:
        raise ValueError("Binding input changed while Clang was parsing")
    for dependency in dependencies:
        if dependency["bindings"]["target"] != bindings["target"]:
            raise ValueError("Binding dependency target mismatch")
    inputs = {Path(path).resolve(strict=True) for path in bindings["inputs"]}
    inputs.update({config_path, config["compilation_database"], config["translation_unit"],
                   generator, formatter, Path(__file__).resolve(), style})
    inputs.update(config["headers"])
    inputs.update(dependency_paths)
    for path in dependency_paths:
        dependency = verify_manifest(path)
        inputs.update(path.parent / name for name in dependency["products"])
    input_paths = sorted(inputs)
    input_hashes = {str(path): digest(path) for path in input_paths}
    # Every C++ translation unit must match the same traits header at compile time.
    token = hashlib.sha256(canonical({"inputs": input_hashes, "bindings": bindings,
                                      "products": products})).hexdigest()
    namespace = bindings["registration"].removesuffix("::Register")
    products[f"{module}_bindings.h"] += (
        f'namespace {namespace} {{ inline constexpr std::string_view kGeneration = "{token}"; }}\n')
    for name in sources:
        products[name] += f'static_assert({namespace}::kGeneration == "{token}");\n'
    products[f"{module}.vela"] = f"// Binding generation: {token}\n" + products[f"{module}.vela"]
    encoded = {name: (format_cpp(text, formatter, style) if name.endswith((".h", ".cc"))
                       else text.encode("utf-8")) for name, text in products.items()}
    # Ninja 使用构建目录内的相对目标；编译数据库固定该目录，避免依赖调用者的 cwd。
    depfile_target = Path(os.path.relpath(manifest_path, config["compilation_database"].parent))
    encoded[f"{module}.d"] = (
        depfile_escape(depfile_target) + ": " + " ".join(depfile_escape(path) for path in input_paths)
        + "\n").encode("utf-8")
    if not check:
        output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".vela-bindgen-", dir=None if check else output) as temporary:
        stage = Path(temporary)
        for name, data in encoded.items():
            (stage / name).write_bytes(data)
        hashed = fingerprints(generator, [stage / name for name in encoded])
        manifest = {
            "format_version": FORMAT_VERSION, "bindings": bindings,
            "binding_generation": token, "registration": namespace + "::Register",
            "fingerprints": {name: hashed[str(stage / name)] for name in encoded},
            "dependencies": [str(path) for path in dependency_paths],
            "inputs": input_hashes,
            "products": {name: hashlib.sha256(data).hexdigest() for name, data in encoded.items()},
        }
        manifest["generation"] = hashlib.sha256(canonical(manifest)).hexdigest()
        manifest_data = json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
        # Stage the manifest before replacing any product and publish it last.
        encoded[manifest_path.name] = manifest_data.encode("utf-8")
        if not check:
            (stage / manifest_path.name).write_bytes(encoded[manifest_path.name])
            for name, expected in input_hashes.items():
                if digest(Path(name)) != expected:
                    raise ValueError(f"Binding input changed during generation: {name}")
        for name, data in encoded.items():
            destination = output / name
            if destination.is_file() and destination.read_bytes() == data:
                continue
            if check:
                raise ValueError(f"Binding output is missing or stale: {destination}")
            os.replace(stage / name, destination)
    verify_manifest(manifest_path)
    return manifest_path


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--config", type=Path)
    mode.add_argument("--verify-manifest", type=Path)
    parser.add_argument("--variant", choices=("debug", "release"), default="debug")
    parser.add_argument("--package-root", type=Path)
    parser.add_argument("--generator", type=Path)
    parser.add_argument("--resource-dir", type=Path)
    parser.add_argument("--formatter", type=Path)
    parser.add_argument("--style", type=Path)
    parser.add_argument("--check", action="store_true")
    parsed = parser.parse_args(arguments)
    if parsed.verify_manifest:
        verify_manifest(parsed.verify_manifest)
    else:
        formatter = parsed.formatter
        style = parsed.style
        if parsed.package_root:
            package = parsed.package_root.resolve(strict=True)
            toolchain = read_object(package / "share/vela/bindgen.json")
            if toolchain.get("format_version") != FORMAT_VERSION:
                raise ValueError("Unsupported packaged binding toolchain")
            generator = package / toolchain["generator"]
            resource = package / toolchain["resource_dir"]
            formatter = package / toolchain["formatter"]
            style = package / toolchain["style"]
            for name, expected in toolchain["files"].items():
                if digest(package / name) != expected:
                    raise ValueError(f"Packaged binding tool is missing or corrupt: {name}")
        else:
            from tools.clang_sdk import resource_directory
            from tools.config import executable_name, output_directory
            generator = parsed.generator or output_directory(parsed.variant) / executable_name("vela_bindgen")
            resource = parsed.resource_dir or resource_directory()
        path = generate(parsed.config, generator, resource, check=parsed.check,
                        formatter=formatter, style=style)
        print(f"Binding generation {'verified' if parsed.check else 'published'}: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"bindgen failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
