#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Stage a self-contained Linux city development bundle; never overwrite an export.

The audited hello-game exporter stays a separate profile. Reuse its payload integrity,
notice collection and system ABI checks, with an explicit city profile and art inventory.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile

from game_bundle_manifest import create, verify
from stage_game_bundle import NOTICE_FIELDS, RECORD_FIELDS, notices, record, regular, requirements, sha256

ROOT = Path(__file__).resolve().parent.parent


def catalog(folder: Path, *, weapons: bool = False) -> None:
    """Manifest targets are relative payload files, never build/source filesystem paths."""
    regular(folder / "manifest.txt")
    names = []
    for line in (folder / "manifest.txt").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise ValueError(f"invalid cooked manifest: {folder}")
        target = PurePosixPath(fields[3])
        if target.is_absolute() or ".." in target.parts or "\\" in fields[3] or ":" in fields[3]:
            raise ValueError("nonrelative cooked target")
        regular(folder / target)
        names.append(fields[0])
    if not names:
        raise ValueError("empty cooked manifest")
    if weapons:
        for name in ["held-scar-h", "held-m4a1", "held-msr", "arms-scar-h", "arms-m4a1", "arms-msr"]:
            if not any(PurePosixPath(n).name.startswith(name + ".") and "#" not in n for n in names):
                raise ValueError(f"missing prepared weapon/arms: {name}")
    for path in folder.rglob("*"):
        if path.is_symlink() or (not path.is_file() and not path.is_dir()):
            raise ValueError(f"not regular cooked content: {path}")


def stage(build: Path, scene: Path, destination: Path) -> tuple[Path, str]:
    if sys.platform != "linux":
        raise ValueError("city development export currently supports native Linux only")
    build, scene, destination = build.resolve(), scene.resolve(), destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("choose a new export directory; existing exports are never overwritten")
    if destination.resolve().is_relative_to(ROOT):
        raise ValueError("export outside the repository")
    regular(scene)
    records = list((build / "export").glob("*/the_block.export"))
    if len(records) != 1:
        raise ValueError("expected one configured city export record")
    data = record(records[0], RECORD_FIELDS)
    if data["schema"] != "2" or data["target"] != "the_block" or data["system"] != "Linux":
        raise ValueError("not the native city export profile")
    binary = Path(data["executable"])
    regular(binary)
    if not os.access(binary, os.X_OK):
        raise ValueError("city executable is not executable")
    stamp = json.loads(Path(str(binary) + ".provenance.json").read_text())
    if stamp.get("schema_version") != 1 or stamp.get("binary_sha256") != sha256(binary):
        raise ValueError("stale build provenance; rebuild the game")
    host = requirements(binary, data["architecture"])
    # This mode is not the hello-game dedicated-server contract. Record only actual city modes.
    host.pop("dedicated_graphics", None)
    host["qualification"] = "development artifact; inherited block/performance gates remain open"
    cooked = build / "samples/99-the-block/cooked"
    viewmodels = build / "samples/99-the-block/viewmodel-cooked"
    catalog(cooked)
    catalog(viewmodels, weapons=True)  # a requested weapon bundle must not silently omit the guns
    audited = record(Path(data["bundle_metadata"]), NOTICE_FIELDS)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".rime-city-stage-", dir=destination.parent))
    try:
        shutil.copy2(binary, temporary / "the_block")
        if sha256(temporary / "the_block") != stamp["binary_sha256"]:
            raise ValueError("executable changed during export")
        content = temporary / "content"
        content.mkdir()
        shutil.copy2(scene, content / "city.rscene")
        shutil.copytree(cooked, content / "block")
        shutil.copytree(viewmodels, content / "viewmodels")
        required = notices(audited, temporary, profile="city-dev-linux-vulkan-v1", title="Rime city development demo")
        art = temporary / "LICENSES/viewmodel"
        art.mkdir()
        source = ROOT / "samples/99-the-block/assets/viewmodel"
        # The attribution document includes the license URI, creators, originals and changes.
        for original, name in [(source / "ASSET-LICENSES.md", "ASSET-LICENSES.md")]:
            regular(original)
            shutil.copyfile(original, art / name)
            required.append("LICENSES/viewmodel/" + name)
        with (temporary / "LICENSES/CONTENTS.txt").open("a") as inventory:
            inventory.write("\nCity simulation, audio, networking, destruction and generated geometry: Rime Apache-2.0.\n"
                            "Prepared weapon/arms art: CC-BY-4.0; creators, source links and modifications in viewmodel/ASSET-LICENSES.md.\n"
                            "No streaming codecs or editor binaries are included in this runtime artifact.\n")
        (temporary / "SYSTEM-REQUIREMENTS.txt").write_text("\n".join(f"{k}: {v}" for k, v in sorted(host.items())) + "\n")
        (temporary / "README.txt").write_text(
            "Rime city + weapons — Linux development build\n\n"
            "Run ./the_block (or ./the_block --play). Content resolves beside the binary.\n"
            "WASD move; mouse look/right-drag fallback; left mouse fire; Q/right mouse aim;\n"
            "R reload; 1 SCAR-H / 2 M4A1 / 3 MSR; F detonates; Escape quits.\n"
            "A bounded functional run: ./the_block --play --frames 12 --ppm screenshot.ppm\n"
            "--content DIR selects a different complete content root. Missing content is an error.\n\n"
            "This is a development bundle. Performance and inherited block-collapse gates remain open;\n"
            "clean second-machine compatibility and a production installer are not established.\n"
            "Read SYSTEM-REQUIREMENTS.txt, LICENSE, NOTICE and LICENSES/CONTENTS.txt.\n"
            "Weapon creators, original links and modifications: LICENSES/viewmodel/ASSET-LICENSES.md.\n"
            "Shaders are embedded. No checkout, editor, Python or Conan is required at runtime.\n"
            f"Build source base: {stamp['source_revision']}; tracked dirty: {stamp['source_dirty']}.\n"
            f"Binary SHA-256: {stamp['binary_sha256']}.\n")
        digest = create(temporary, {
            "game_id": "rime-city-dev", "target_triple": "x86_64-linux-gnu",
            "configuration": data["configuration"], "executable": "the_block",
            "source_revision": stamp["source_revision"], "source_dirty": stamp["source_dirty"],
            "network_protocol": "local scripted client/server; no public listening server",
            "supported_modes": ["play"], "requirements": host, "required_notices": required,
        })
        verify(temporary, digest)
        if destination.exists():
            raise ValueError("export destination appeared during staging")
        os.rename(temporary, destination)
        return destination, digest
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/release")
    args = parser.parse_args()
    try:
        bundle, digest = stage(args.build, args.scene, args.destination)
        print(f"City development bundle: {bundle}\nManifest SHA-256: {digest}")
        return 0
    except (ValueError, OSError, KeyError) as error:
        print(f"city export: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
