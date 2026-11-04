"""Run the binding command as a GN action with the repository module path."""

from pathlib import Path
import runpy
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

if __name__ == "__main__":
    runpy.run_module("tools.bindgen", run_name="__main__")
