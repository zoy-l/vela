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
    expected = {f"{module}.vela", f"{module}_bindings.h", f"{module}_bindings.cc", f"{module}.d"}
    products = manifest.get("products", {})
    if not isinstance(products, dict) or set(products) != expected:
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
    if not required <= config.keys() or config.keys() - required - {"dependencies"}:
        raise ValueError("Binding config requires module, headers, translation_unit, "
                         "compilation_database, output, and optional dependencies")
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
    return config


def depfile_escape(path: Path) -> str:
    return path.as_posix().replace("$", "$$").replace(" ", "\\ ").replace("#", "\\#").replace(":", "\\:")


def format_cpp(text: str, formatter: Path, style: Path) -> bytes:
    result = subprocess.run([str(formatter), "--style=file:" + str(style)],
                            input=text, capture_output=True, text=True, encoding="utf-8", check=True)
    return result.stdout.replace("\r\n", "\n").encode("utf-8")


def fingerprints(generator: Path, paths: list[Path]) -> dict[str, str]:
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
    dependencies = [verify_manifest(path) for path in config["dependencies"]]
    module = config["module"]
    output = config["output"]
    manifest_path = output / f"{module}.bindings.json"
    generator = generator.resolve(strict=True)
    arguments = [str(generator), "--module=" + module,
                 "--database=" + str(config["compilation_database"]),
                 "--source=" + str(config["translation_unit"]),
                 "--resource-dir=" + str(resource.resolve(strict=True)),
                 "--output-directory=" + str(output),
                 *["--header=" + str(path) for path in config["headers"]],
                 *["--dependency=" + str(path) for path in config["dependencies"]]]
    result = subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8", check=False)
    if result.returncode:
        raise ValueError(result.stderr.strip() or "Binding collection failed")
    bindings = json.loads(result.stdout)
    products = bindings.pop("products")
    parsed_fingerprints = bindings["input_fingerprints"]
    current_fingerprints = fingerprints(generator, [Path(path) for path in parsed_fingerprints])
    if current_fingerprints != parsed_fingerprints:
        raise ValueError("Binding input changed while Clang was parsing")
    for dependency in dependencies:
        if dependency["bindings"]["target"] != bindings["target"]:
            raise ValueError("Binding dependency target mismatch")
    inputs = {Path(path).resolve(strict=True) for path in bindings["inputs"]}
    inputs.update({config_path, config["compilation_database"], config["translation_unit"],
                   generator, formatter, Path(__file__).resolve(), style})
    inputs.update(config["headers"])
    inputs.update(config["dependencies"])
    for path in config["dependencies"]:
        dependency = verify_manifest(path)
        inputs.update(path.parent / name for name in dependency["products"])
    input_hashes = {str(path): digest(path) for path in sorted(inputs)}
    # This token couples the C++ translation unit to its traits header at compile time.
    token = hashlib.sha256(canonical({"inputs": input_hashes, "bindings": bindings,
                                      "products": products})).hexdigest()
    namespace = bindings["registration"].removesuffix("::Register")
    products[f"{module}_bindings.h"] += (
        f'namespace {namespace} {{ inline constexpr std::string_view kGeneration = "{token}"; }}\n')
    products[f"{module}_bindings.cc"] += f'static_assert({namespace}::kGeneration == "{token}");\n'
    products[f"{module}.vela"] = f"// Binding generation: {token}\n" + products[f"{module}.vela"]
    encoded = {name: (format_cpp(text, formatter, style) if name.endswith((".h", ".cc"))
                       else text.encode("utf-8")) for name, text in products.items()}
    encoded[f"{module}.d"] = (
        depfile_escape(manifest_path) + ": " + " ".join(depfile_escape(path) for path in sorted(inputs))
        + "\n").encode("utf-8")
    with tempfile.TemporaryDirectory(prefix="vela-binding-fingerprints-") as temporary:
        stage = Path(temporary)
        for name, data in encoded.items():
            (stage / name).write_bytes(data)
        hashed = fingerprints(generator, [stage / name for name in encoded])
        product_fingerprints = {name: hashed[str(stage / name)] for name in encoded}
    manifest = {"format_version": FORMAT_VERSION, "bindings": bindings,
                "binding_generation": token, "fingerprints": product_fingerprints,
                "registration": namespace + "::Register",
                "dependencies": [str(path) for path in config["dependencies"]],
                "inputs": input_hashes,
                "products": {name: hashlib.sha256(data).hexdigest() for name, data in encoded.items()}}
    manifest["generation"] = hashlib.sha256(canonical(manifest)).hexdigest()
    encoded[manifest_path.name] = (json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2)
                                   + "\n").encode("utf-8")
    if check:
        for name, data in encoded.items():
            product = output / name
            if not product.is_file() or product.read_bytes() != data:
                raise ValueError(f"Binding output is missing or stale: {product}")
        verify_manifest(manifest_path)
        return manifest_path
    output.mkdir(parents=True, exist_ok=True)
    # Stage the whole generation before replacing any product. Publish the manifest last;
    # consumers verify all product hashes and never trust a partially replaced generation.
    with tempfile.TemporaryDirectory(prefix=".vela-bindgen-", dir=output) as temporary:
        stage = Path(temporary)
        for name, data in encoded.items():
            (stage / name).write_bytes(data)
        for name, expected in input_hashes.items():
            if digest(Path(name)) != expected:
                raise ValueError(f"Binding input changed during generation: {name}")
        for name in encoded:
            destination = output / name
            if not destination.is_file() or destination.read_bytes() != encoded[name]:
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
    parser.add_argument("--check", action="store_true")
    parsed = parser.parse_args(arguments)
    if parsed.verify_manifest:
        verify_manifest(parsed.verify_manifest)
    else:
        formatter = None
        style = None
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
            from tools.clang_sdk import sdk_directory
            from tools.config import executable_name, output_directory
            generator = parsed.generator or output_directory(parsed.variant) / executable_name("vela_bindgen")
            resource = parsed.resource_dir or sdk_directory() / "lib/clang/21"
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
