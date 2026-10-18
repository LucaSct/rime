#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Exercise the real saved-city generator/runtime seam without a GPU or collapse thresholds."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main() -> None:
    game = Path(sys.argv[1]).resolve()
    def run(*args: str, expected: int = 0) -> str:
        result = subprocess.run([str(game), *args], text=True, capture_output=True, timeout=60)
        if result.returncode != expected:
            raise AssertionError(f"{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
        return result.stdout + result.stderr
    def digest(path: Path) -> str:
        output = run("--scene-info", "--scene", str(path))
        found = re.search(r"digest (0x[0-9a-f]+), city-patterns=1, no GPU", output)
        assert found, output
        return found[1]
    with tempfile.TemporaryDirectory(prefix="rime city scene ") as directory:
        folder = Path(directory)
        first, second, edited = [folder / name for name in ["first.rscene", "second.rscene", "edited map.rscene"]]
        run("--write-city", str(first))
        run("--write-city", str(second))
        assert first.read_bytes() == second.read_bytes(), "city generation must be repeatable"
        original = hashlib.sha256(first.read_bytes()).digest()
        run("--write-city", str(first), expected=2)
        assert hashlib.sha256(first.read_bytes()).digest() == original, "generator overwrote authored data"
        text, count = re.subn(r"translation \{ x [-.0-9e+]+", "translation { x 22.25", first.read_text(), count=1)
        assert count == 1
        edited.write_text(text)
        assert digest(first) != digest(edited), "runtime ignored the edited map"
        # Unknown destruction content must refuse, never silently omit a building.
        unknown = folder / "unknown.rscene"
        text, count = re.subn(r"(rime::destruction::Destructible[^}]*?asset )\d+", r"\g<1>18446744073709551614", first.read_text(), count=1)
        assert count == 1
        unknown.write_text(text)
        run("--scene-info", "--scene", str(unknown), expected=2)
        print("city scene seam OK: deterministic generation, strict pattern binding, edited digest, overwrite refusal")


if __name__ == "__main__":
    main()
