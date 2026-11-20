"""Packaged native binding command; requires only the host's Python interpreter."""

from pathlib import Path
import runpy
import sys

package = Path(__file__).resolve().parents[1]
sys.argv[1:1] = ["--package-root", str(package)]
runpy.run_path(str(package / "tools" / "bindgen.py"), run_name="__main__")
