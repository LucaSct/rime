#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Stage and verify the reviewed Linux hello-game export before replacing an old bundle.

This is the existing exporter's copy/dependency step, not a package manager. Build records
are trusted local inputs. The resulting manifest verifies bytes; it does not sign a publisher.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import subprocess
import tempfile

from game_bundle_manifest import create, verify


REFERENCES = {
    "fmt": "fmt/10.2.1",
    "volk": "volk/1.4.313.0",
    "vulkan-memory-allocator": "vulkan-memory-allocator/3.3.0",
    "vulkan-headers": "vulkan-headers/1.4.313.0",
}
REQUIRED_TEXTS = {
    "fmt": ["LICENSE"],
    "volk": ["LICENSE.md"],
    "vulkan-memory-allocator": ["LICENSE.txt"],
    "vulkan-headers": ["LICENSE.md", "LICENSES/Apache-2.0.txt", "LICENSES/MIT.txt"],
}
PROTOCOLS = ["xdg-shell", "pointer-constraints-unstable-v1", "relative-pointer-unstable-v1"]
RECORD_FIELDS = {
    "schema", "target", "executable", "content", "configuration", "system",
    "architecture", "project_version", "bundle_metadata",
}
NOTICE_FIELDS = {"profile", "notice_rime", "notice_rime_notice", "wayland_generated"} | {
    prefix + component for component in REFERENCES for prefix in ["notice_", "reference_"]
}


def record(path: Path, fields: set[str]) -> dict[str, str]:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if not sep or key not in fields or key in result or not value:
            raise ValueError(f"invalid or duplicate export field in {path.name}: {key!r}")
        result[key] = value
    if result.keys() != fields:
        raise ValueError(f"incomplete export record {path.name}: {sorted(fields - result.keys())}")
    return result


def regular(path: Path) -> None:
    if not stat.S_ISREG(path.lstat().st_mode):
        raise ValueError(f"not a regular file: {path}")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(*args: str) -> str:
    env = {k: v for k, v in os.environ.items() if k not in {"LD_PRELOAD", "LD_LIBRARY_PATH"}}
    env["LC_ALL"] = "C"
    return subprocess.run(args, check=True, text=True, capture_output=True, env=env).stdout


def requirements(binary: Path, architecture: str) -> dict[str, str]:
    header = run("readelf", "-h", "--", str(binary))
    if architecture.lower() not in {"x86_64", "amd64"} or "Advanced Micro Devices X86-64" not in header:
        raise ValueError("this reviewed export profile supports Linux x86_64 ELF only")
    dynamic = run("readelf", "-d", "--", str(binary))
    needed = sorted(re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic))
    if not needed or any("vulkan" in name.lower() for name in needed):
        raise ValueError("expected dynamic system dependencies and no link-time Vulkan loader")
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        raise ValueError("the game binary carries an RPATH/RUNPATH")
    linked = run("ldd", "--", str(binary))
    if "not found" in linked:
        raise ValueError("the game has unresolved shared-library dependencies")
    paths = re.findall(r"(?:=>\s+|^\s*)(/\S+)", linked, flags=re.M)
    if not paths:
        raise ValueError("ldd did not identify resolved system libraries")
    system_roots = [Path(p) for p in ["/lib", "/lib64", "/usr/lib", "/usr/lib64"]]
    for path in paths:
        resolved = Path(path).resolve(strict=True)
        if not any(resolved.is_relative_to(root) for root in system_roots):
            raise ValueError(f"non-system shared library: {resolved}")
    versions = run("readelf", "--version-info", "--", str(binary))
    result = {
        "os": "Linux", "architecture": "x86_64", "direct_system_libraries": ", ".join(needed),
        "compatibility_evidence": "current build host; clean second-machine validation pending",
        "play_graphics": "Vulkan 1.3 loader and compatible driver; not bundled",
        "dedicated_graphics": "none; window-system shared libraries remain host prerequisites",
    }
    for prefix in ["GLIBC", "GLIBCXX", "CXXABI"]:
        matches = set(re.findall(r"\b" + prefix + r"_(\d+(?:\.\d+)+)\b", versions))
        if matches:
            result[prefix.lower() + "_required"] = max(matches, key=lambda s: tuple(map(int, s.split("."))))
    if "glibc_required" not in result:
        raise ValueError("could not determine the Linux runtime ABI requirement")
    return result


def notices(meta: dict[str, str], destination: Path) -> list[str]:
    if meta["profile"] != "hello-game-linux-vulkan-v1":
        raise ValueError("no reviewed redistribution profile for this game")
    required = ["LICENSE", "NOTICE", "LICENSES/CONTENTS.txt"]
    destination.joinpath("LICENSES").mkdir()
    inventory = [
        "hello-game Linux/Vulkan redistribution inventory, profile v1.",
        "The root NOTICE is Rime's broader attribution document; this inventory names this artifact.",
        "System shared libraries and Vulkan drivers are host prerequisites, not copied payloads.",
        "Tool-only dependencies and streaming codecs are not incorporated in this target.", "",
        "Rime engine, sample, authored shaders and arena content: Apache-2.0; see ../LICENSE and ../NOTICE.",
    ]
    for key, name in [("notice_rime", "LICENSE"), ("notice_rime_notice", "NOTICE")]:
        source = Path(meta[key])
        regular(source)
        shutil.copyfile(source, destination / name)
    for component, reference in REFERENCES.items():
        if meta["reference_" + component] != reference:
            raise ValueError(f"redistribution inventory needs review for {component}'s new version")
        source = Path(meta["notice_" + component])
        if source.is_symlink() or not source.is_dir():
            raise ValueError(f"invalid notice directory: {component}")
        for text in REQUIRED_TEXTS[component]:
            regular(source / text)
            if (source / text).stat().st_size == 0:
                raise ValueError(f"empty required notice: {component}/{text}")
            required.append(f"LICENSES/{component}/{text}")
        shutil.copytree(source, destination / "LICENSES" / component, symlinks=True)
        inventory.append(f"{reference}: authentic package notice directory in {component}/.")
    protocol_destination = destination / "LICENSES" / "wayland"
    protocol_destination.mkdir()
    for protocol in PROTOCOLS:
        source = Path(meta["wayland_generated"]) / (protocol + "-protocol.c")
        regular(source)
        code = source.read_text(encoding="utf-8")
        blocks = [b for b in re.findall(r"/\*.*?\*/", code, flags=re.S)
                  if "Copyright" in b and "Permission is hereby granted" in b and "THE SOFTWARE IS PROVIDED" in b]
        if len(blocks) != 1:
            raise ValueError(f"missing or ambiguous generated-protocol notice: {protocol}")
        path = f"LICENSES/wayland/{protocol}.txt"
        destination.joinpath(path).write_text(blocks[0] + "\n", encoding="utf-8")
        required.append(path)
        inventory.append(f"Wayland {protocol}: verbatim copyright/permission block in wayland/{protocol}.txt.")
    destination.joinpath("LICENSES/CONTENTS.txt").write_text("\n".join(inventory) + "\n", encoding="utf-8")
    return required


def stage(export_record: Path, out_root: Path, repository: Path) -> tuple[Path, str]:
    data = record(export_record, RECORD_FIELDS)
    if data["schema"] != "2" or data["target"] != "hello_game" or data["system"] != "Linux":
        raise ValueError("only the reviewed Linux hello_game export is implemented")
    binary, content = Path(data["executable"]), Path(data["content"])
    regular(binary)
    if not os.access(binary, os.X_OK) or not content.is_dir() or content.is_symlink():
        raise ValueError("the export needs an executable and a regular content directory")
    provenance_path = Path(str(binary) + ".provenance.json")
    regular(provenance_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    if (not isinstance(provenance, dict) or set(provenance) != {"schema_version", "binary_sha256", "source_revision", "source_dirty"}
            or type(provenance["schema_version"]) is not int or provenance["schema_version"] != 1
            or not isinstance(provenance["source_revision"], str)
            or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", provenance["source_revision"])
            or type(provenance["source_dirty"]) is not bool
            or provenance["binary_sha256"] != sha256(binary)):
        raise ValueError("missing, stale or unknown build provenance; rebuild the game before exporting")
    audited = record(Path(data["bundle_metadata"]), NOTICE_FIELDS)
    host_requirements = requirements(binary, data["architecture"])
    out_root = out_root.resolve()
    final = out_root / data["target"]
    if final.is_relative_to(repository.resolve()):
        raise ValueError("the bundle must be written outside the repository")
    if final.is_symlink() or (final.exists() and not final.is_dir()):
        raise ValueError("the destination exists and is not a regular bundle directory")
    out_root.mkdir(parents=True, exist_ok=True)
    staged = Path(tempfile.mkdtemp(prefix=".hello_game.stage-", dir=out_root))
    backup = None
    published = False
    try:
        shutil.copyfile(binary, staged / data["target"])
        (staged / data["target"]).chmod(0o755)
        if sha256(staged / data["target"]) != provenance["binary_sha256"]:
            raise ValueError("the executable changed while it was being staged")
        shutil.copytree(content, staged / "content", symlinks=True)
        required_notices = notices(audited, staged)
        staged.joinpath("SYSTEM-REQUIREMENTS.txt").write_text(
            "\n".join(f"{k}: {v}" for k, v in sorted(host_requirements.items())) + "\n", encoding="utf-8")
        staged.joinpath("README.txt").write_text(
            "hello-game — a native Linux game built with Rime.\n\n"
            "  ./hello_game play                         windowed play\n"
            "  ./hello_game play --headless               off-screen/simulation fallback\n"
            "  ./hello_game dedicated --ticks 120 --autopilot\n"
            "                                            GPU-free bounded simulation\n"
            "  ./hello_game --help                       options\n\n"
            "Dedicated networking and browser/stream/host launch modes are not implemented.\n"
            "This is a development export; clean second-machine compatibility is not established.\n"
            "Read SYSTEM-REQUIREMENTS.txt, LICENSE, NOTICE and LICENSES/CONTENTS.txt.\n"
            "Shaders are embedded; content resolves beside the executable or via --content.\n"
            f"Linked source base: {provenance['source_revision']}; tracked source dirty: {provenance['source_dirty']}.\n"
            f"Executable SHA-256: {provenance['binary_sha256']}.\n"
            "rime-game.toml inventories the exact payload; unsigned hashes do not authenticate its publisher.\n",
            encoding="utf-8")
        metadata = {
            "game_id": data["target"], "target_triple": "x86_64-linux-gnu",
            "configuration": data["configuration"], "executable": data["target"],
            "source_revision": provenance["source_revision"], "source_dirty": provenance["source_dirty"],
            "network_protocol": "not-networked", "supported_modes": ["play", "dedicated"],
            "requirements": host_requirements, "required_notices": required_notices,
        }
        digest = create(staged, metadata)
        verify(staged, digest)
        # Verify before publishing. Retain an existing good export until every dependency,
        # notice and payload check has succeeded; restore it if the final rename fails.
        if final.exists():
            backup = Path(tempfile.mkdtemp(prefix=".hello_game.previous-", dir=out_root))
            os.replace(final, backup / "payload")
        try:
            os.replace(staged, final)
            published = True
        except OSError as publish_error:
            if backup is not None:
                try:
                    os.replace(backup / "payload", final)
                except OSError as restore_error:
                    raise OSError(f"publication failed ({publish_error}); restoration failed "
                                  f"({restore_error}); previous bundle retained at {backup / 'payload'}") from restore_error
            raise
        return final, digest
    finally:
        if staged.exists():
            shutil.rmtree(staged)
        if backup is not None and (published or not (backup / "payload").exists()):
            shutil.rmtree(backup)
        # If restoring the old directory itself failed, retain the backup for recovery.


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    parser.add_argument("out_root", type=Path)
    parser.add_argument("--repository", required=True, type=Path)
    args = parser.parse_args()
    try:
        bundle, digest = stage(args.record, args.out_root, args.repository)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"export-game: FAILED: {error}", file=sys.stderr)
        return 1
    print(f"export-game: verified bundle {bundle}\nexport-game: manifest SHA-256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
