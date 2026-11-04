"""Packaged native binding command; requires only the host's Python interpreter."""

from pathlib import Path
import runpy
import sys

package = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(package))
sys.argv[1:1] = ["--package-root", str(package)]
runpy.run_module("tools.bindgen", run_name="__main__")
