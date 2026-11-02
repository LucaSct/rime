# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Verify SHA-256 pins for Venice source GLBs, cook them with BC7, and check scene IDs.

Usage:
    python3 assets/tools/cook_venice.py [--src DIR] [--out DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

DEFAULT_VENICE_SRC = "/mnt/tessera/venice-assets/venice_photo_assets/assets/src"

PINNED_GLB_SHA256: dict[str, str] = {
    "boat_hull_only.glb": "2a78959b97d4263c7a8f7b686dfc14306072f35a5ae1f971ae010051d008f2ef",
    "boat_workboat.glb": "987e6056db3799421ef254d9574e6c1844a4f4d1635acb8fe1198e36f738b1a2",
    "cockle_banded.glb": "ccc78c9007eb6420eab988d4df4e2860247be5066dadfaf5c489021f93e4c071",
    "cockle_orange.glb": "f5fa0d77056918e7dbdc01b876788ae9bbf02d852556e41fb61449ad17a72a99",
    "door_planks_transom.glb": "4f79cd4fce0e5199b388ed0a85e0127b7033c1f9b62db111d9fffe8d5b983546",
    "driftwood_log.glb": "fd776e163e22abf3266d2e49bea8be47528c12732a1e63ba89fb7b0d79411d35",
    "grass_clump.glb": "b32dac259dda9ad333c6cf6ff550400d1fe0a261788a65081a7449f4978a2ff0",
    "rose.glb": "b180edabce8b8b3b686852d44d1fd2d8eed06338ed3b3dd6500d4083c91a8453",
    "sand_ground.glb": "d8e9313eff25ed8fb1e59714a194e3df636487776819336866754bdc389a74fb",
    "top_snail.glb": "99c176678316bb0cd47ce9adf2659dc996d9e9ceb9fc24f9f2559a3953996964",
    "venice_intonaco_wall.glb": "a3cbbcdd233be5fe84e2d951dc0c39493aa7961d7254d2b4611f1dbd7f59716f",
    "venice_masegni_ground.glb": "d96ec663684ee9c8c518a6c55089c86e89e8fa5c0d8a720eb5a67209b33cb36e",
    "window_barred_sill.glb": "7e52cb4e90de62d398182b9c4488a811d5ec4a7f0c3c08e6ae0fcc39e2a60740",
    "window_shutters_istrian.glb": "e9573a783b7f92a7a84d0f9b1ecff232551928d47c701f2c68110ceceede4cc2",
}


class SourceVerificationError(Exception):
    """Raised when a source GLB is missing or fails SHA-256 verification (exit code 2)."""


def sha256_of_file(path: Path) -> str:
    """Compute the SHA-256 hexadecimal digest of a file."""
    hasher = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            hasher.update(chunk)
    return hasher.hexdigest()


def verify_sources(src_dir: Path) -> None:
    """Verify every pinned GLB exists under `src_dir` and matches its SHA-256 digest."""
    if not src_dir.is_dir():
        raise SourceVerificationError(
            f"Venice source directory not found: {src_dir}\n"
            f"Pass --src <dir> or set RIME_VENICE_SRC."
        )

    for filename, expected_hash in sorted(PINNED_GLB_SHA256.items()):
        glb_path = src_dir / filename
        if not glb_path.is_file():
            raise SourceVerificationError(
                f"Missing required Venice source GLB: {glb_path}"
            )
        actual_hash = sha256_of_file(glb_path)
        if actual_hash != expected_hash:
            raise SourceVerificationError(
                f"SHA-256 mismatch for {glb_path}:\n"
                f"  Expected: {expected_hash}\n"
                f"  Actual:   {actual_hash}"
            )
        print(f"  [verified] {filename} ({actual_hash[:12]}...)")


def find_rime_cli(repo_root: Path) -> Path:
    """Locate the built `rime` CLI binary."""
    env_override = os.environ.get("RIME_CLI")
    if env_override:
        p = Path(env_override)
        if p.is_file():
            return p
        raise RuntimeError(f"RIME_CLI is set to '{env_override}', which does not exist")

    exe_suffix = ".exe" if os.name == "nt" else ""
    candidates = [
        repo_root / "tools" / "target" / "release" / f"rime{exe_suffix}",
        repo_root / "tools" / "target" / "debug" / f"rime{exe_suffix}",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    which_rime = shutil.which("rime")
    if which_rime:
        return Path(which_rime)

    raise RuntimeError(
        "Built `rime` CLI not found under tools/target/{release,debug}/rime or on PATH. "
        "Run `scripts/build.sh --no-tests` first."
    )


def parse_manifest_ids(manifest_path: Path) -> dict[str, set[int]]:
    """Read `<out>/manifest.txt` and return decimal asset IDs grouped by kind."""
    if not manifest_path.is_file():
        raise RuntimeError(f"Cooked manifest not found at {manifest_path}")

    by_kind: dict[str, set[int]] = {"mesh": set(), "material": set(), "texture": set()}
    with open(manifest_path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                raise RuntimeError(f"Malformed manifest line {lineno} in {manifest_path}: {raw!r}")
            _src, kind, hex_id, _cooked = parts[:4]
            asset_id = int(hex_id, 16)
            by_kind.setdefault(kind, set()).add(asset_id)
    return by_kind


def parse_scene_asset_ids(scene_path: Path) -> tuple[set[int], set[int]]:
    """Extract all `MeshAsset` and `MaterialAsset` decimal IDs from an `.rscene`."""
    if not scene_path.is_file():
        raise RuntimeError(f"Scene file not found at {scene_path}")

    text = scene_path.read_text(encoding="utf-8")
    mesh_pattern = re.compile(
        r"rime::render::MeshAsset\s+0x[0-9a-fA-F]+\s*\{\s*asset\s+(\d+)\s*\}",
        re.MULTILINE,
    )
    mat_pattern = re.compile(
        r"rime::render::MaterialAsset\s+0x[0-9a-fA-F]+\s*\{\s*asset\s+(\d+)\s*\}",
        re.MULTILINE,
    )
    mesh_ids = {int(m.group(1), 10) for m in mesh_pattern.finditer(text)}
    mat_ids = {int(m.group(1), 10) for m in mat_pattern.finditer(text)}
    return mesh_ids, mat_ids


def verify_scene_ids(scene_path: Path, manifest_path: Path) -> None:
    """Verify every MeshAsset and MaterialAsset ID in `scene_path` exists in `manifest_path`."""
    manifest_ids = parse_manifest_ids(manifest_path)
    mesh_ids, mat_ids = parse_scene_asset_ids(scene_path)
    if not mesh_ids and not mat_ids:
        raise RuntimeError(f"No MeshAsset or MaterialAsset components found in {scene_path}")

    missing: list[str] = []
    for mid in sorted(mesh_ids):
        if mid not in manifest_ids.get("mesh", set()):
            missing.append(f"MeshAsset {mid} (0x{mid:016x})")
    for mat_id in sorted(mat_ids):
        if mat_id not in manifest_ids.get("material", set()):
            missing.append(f"MaterialAsset {mat_id} (0x{mat_id:016x})")

    if missing:
        raise RuntimeError(
            f"{len(missing)} scene asset ID(s) from {scene_path} missing in {manifest_path}:\n  "
            + "\n  ".join(missing)
        )

    print(
        f"--> Verified {len(mesh_ids)} distinct MeshAsset ID(s) and "
        f"{len(mat_ids)} distinct MaterialAsset ID(s) in {scene_path.relative_to(scene_path.parent.parent.parent)}."
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent.parent
    default_src = Path(os.environ.get("RIME_VENICE_SRC", DEFAULT_VENICE_SRC))
    default_out = repo_root / "build" / "asset-cache" / "venice"
    scene_path = repo_root / "samples" / "venice" / "venice.rscene"

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--src",
        type=Path,
        default=default_src,
        help=f"Directory containing the 14 Venice source GLBs (default: $RIME_VENICE_SRC or {DEFAULT_VENICE_SRC})",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=default_out,
        help=f"Output directory for cooked files and manifest.txt (default: {default_out})",
    )
    args = parser.parse_args()

    print(f"--> Verifying {len(PINNED_GLB_SHA256)} Venice source GLBs in {args.src}:")
    verify_sources(args.src)

    rime_cli = find_rime_cli(repo_root)
    args.out.mkdir(parents=True, exist_ok=True)

    # One cook invocation over ONE staging directory: the 14 private GLBs (symlinked, never
    # copied) plus the committed, code-generated canal water glTF. It has to be a single
    # invocation because the cook cache reflects the input set of the call that wrote it -- a
    # second `rime cook` into the same --out would drop the first call's sources from the
    # manifest.
    stage = args.out.parent / (args.out.name + "-src")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for filename in sorted(PINNED_GLB_SHA256):
        (stage / filename).symlink_to((args.src / filename).resolve())
    water = repo_root / "samples" / "venice" / "canal_water.gltf"
    if not water.is_file():
        raise SourceVerificationError(
            f"{water} is missing; regenerate it with python3 samples/venice/make_water.py"
        )
    (stage / water.name).symlink_to(water)

    cmd = [str(rime_cli), "cook", str(stage), "--out", str(args.out), "--bc"]
    print(f"--> Running cook:\n    {' '.join(cmd)}")
    proc = subprocess.run(cmd, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"`rime cook` failed with exit code {proc.returncode}")

    manifest_path = args.out / "manifest.txt"
    verify_scene_ids(scene_path, manifest_path)
    print("--> Venice cook and manifest verification succeeded.")


if __name__ == "__main__":
    try:
        main()
    except SourceVerificationError as exc:
        sys.stderr.write(f"cook_venice.py: SOURCE ERROR: {exc}\n")
        sys.exit(2)
    except Exception as exc:
        sys.stderr.write(f"cook_venice.py: ERROR: {exc}\n")
        sys.exit(1)
