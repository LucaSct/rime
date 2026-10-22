# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Fetch Poly Haven source assets at build time and export them to GLB.

Usage:
    python3 assets/tools/fetch_props.py [options] [asset_id ...]

Why this script exists (architectural & engineering principles):
  1. No binaries in git history:
     Git is optimized for text deltas. Binary formats like .blend and .glb cannot be diffed
     line-by-line; every revision stores an entire copy of the binary in the repository's
     object database. Checking in 11.5 MB of GLB files inflates clone sizes and network
     transfer overhead permanently for every contributor across all future branches.
  2. Cryptographic hash pinning (supply chain integrity):
     Poly Haven is an external upstream public repository. Without cryptographic verification,
     upstream changes, server compromise, or network tampering could introduce untrusted data
     or silently modify asset topology and material properties. Every downloaded .blend and
     texture file is verified against an immutable SHA-256 digest pinned directly in this
     script. Any hash mismatch immediately halts execution and removes any incomplete download.
  3. Content caching (fast, repeatable builds):
     Downloading ~10 MB of textures and models on every build wastes bandwidth and slows developer
     iteration. Downloaded files are stored in `build/asset-cache/<id>/` (which is covered by
     the root `.gitignore`). If a cached file exists and its SHA-256 digest matches the pinned
     hash, the network download is skipped entirely.
  4. Single fixed resolution (1k):
     Poly Haven provides assets across multiple resolutions (1k, 2k, 4k, 8k). We select 1k:
     - 1k textures (1024x1024) exactly match the authoring budget and specifications defined in
       `docs/authoring-blender.md` §2 ("Every 1024² texture compresses from 5,592,620 bytes to
       1,398,344 bytes under BC7").
     - 1k keeps source downloads compact (~10 MB total across all 3 props) and export times fast.
     - 1k produces the exact byte-for-byte cooked meshes and runtime content hashes referenced in
       `samples/m16-blender-props/props.rscene`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.request

# Target resolution for all fetched props.
RESOLUTION = "1k"

# Poly Haven files API endpoint.
POLYHAVEN_API_BASE = "https://api.polyhaven.com/files/{asset_id}"

# Pinned SHA-256 hashes for all downloaded source files per prop.
# Keys are asset IDs; values map relative cache paths to expected SHA-256 hex digests.
PINNED_HASHES: dict[str, dict[str, str]] = {
    "Barrel_02": {
        "Barrel_02_1k.blend": "c28b2a31eb7cef91adb6365b8d1464f840def6731efd3a9600d573502360a90a",
        "textures/Barrel_02_diff_1k.jpg": "2dd828ce25b5b6b0ffc9b7901d1778767f1897be1c8004f4ec047fc91742cf12",
        "textures/Barrel_02_metallic_1k.exr": "9b75a6552f71f10ec4a24ad46f05e7a499a93b3712f09bf663ae1a7db85c01e5",
        "textures/Barrel_02_nor_gl_1k.exr": "4033d2893797270ce9b3fa6d6e76736b73d26ed12f6a6628f39db573cf5fad78",
        "textures/Barrel_02_roughness_1k.jpg": "7d5521606036f59012b838fda96fa1fc22cd66aacda569c938da1f567ee6a51a",
    },
    "wooden_crate_02": {
        "wooden_crate_02_1k.blend": "ea1ada8ccc97bc1a0996110d91867c7824c86a76322af527849db49eec35475e",
        "textures/wooden_crate_02_diff_1k.jpg": "a6e57123110d2899af9493c22b1918174f1c96c707e7126ac67738d98ee10c07",
        "textures/wooden_crate_02_metal_1k.exr": "1884face043e7a09dbe24500711d90e9934100d47eb9bbd5334a6391544a7667",
        "textures/wooden_crate_02_nor_gl_1k.exr": "0878c1994b5fbca301ddb7dcc75f7222f551c93c359be7092a30ac6395d46cad",
        "textures/wooden_crate_02_rough_1k.exr": "5ce2ca0d9e104f3189d47d18c8cafd0adc5054a3f2b2c26439af77fe52cc1651",
    },
    "painted_wooden_bench": {
        "painted_wooden_bench_1k.blend": "3f68590e70b7d0d5c9624b3938350bb917552a4f4d88bfcfb7cfe16885bd4b0e",
        "textures/painted_wooden_bench_diff_1k.jpg": "9bc533313091ee71eaca5fa6334b71d27935d9d75311f64df6a53c28a67c435c",
        "textures/painted_wooden_bench_nor_gl_1k.exr": "14e5b0d83bfa8ed7f07f8f39b3bc508473efba66b8a12d9ff175e774ee4cd848",
        "textures/painted_wooden_bench_rough_1k.exr": "52cb94cb127fe4cf59f9144bcdb35eb883eb843c9f6d4f2a347b78f8a5d4155d",
    },
}


def sha256_of_file(path: Path) -> str:
    """Compute the SHA-256 hexadecimal digest of a file."""
    hasher = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            hasher.update(chunk)
    return hasher.hexdigest()


def fetch_and_verify_file(url: str, dest_path: Path, expected_sha256: str) -> None:
    """Download a file if missing or invalid, verifying against its pinned SHA-256.

    If the file exists and matches `expected_sha256`, the download is skipped.
    If the file does not match or a download fails, temporary files are removed and
    a non-zero exception is raised so no invalid data is left behind.
    """
    if dest_path.is_file():
        existing_hash = sha256_of_file(dest_path)
        if existing_hash == expected_sha256:
            print(f"    [cached] {dest_path.name} (SHA-256 matches: {existing_hash[:12]}...)")
            return
        print(f"    [stale]  {dest_path.name} hash mismatch; removing and re-fetching")
        dest_path.unlink()

    dest_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = dest_path.with_name(dest_path.name + ".part")
    if temp_path.exists():
        temp_path.unlink()

    print(f"    [downloading] {dest_path.name} from {url}...")
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Rime-Asset-Fetcher/1.0 (+https://github.com/rime-engine)"},
    )

    hasher = hashlib.sha256()
    try:
        with urllib.request.urlopen(req) as resp, open(temp_path, "wb") as out_file:
            if resp.status != 200:
                raise RuntimeError(f"HTTP {resp.status} fetching {url}")
            while chunk := resp.read(65536):
                out_file.write(chunk)
                hasher.update(chunk)
    except Exception as e:
        if temp_path.exists():
            temp_path.unlink()
        raise RuntimeError(f"Failed to download {url}: {e}") from e

    actual_hash = hasher.hexdigest()
    if actual_hash != expected_sha256:
        temp_path.unlink()
        raise RuntimeError(
            f"SHA-256 verification failed for {dest_path.name}:\n"
            f"  Expected: {expected_sha256}\n"
            f"  Actual:   {actual_hash}\n"
            f"No invalid file was kept."
        )

    temp_path.replace(dest_path)
    print(f"    [verified]    {dest_path.name} (SHA-256 {actual_hash[:12]}...)")


def fetch_asset_files(asset_id: str, cache_dir: Path) -> Path:
    """Fetch all files required for `asset_id` via the Poly Haven API into `cache_dir`.

    Returns the absolute path to the downloaded `.blend` file.
    """
    if asset_id not in PINNED_HASHES:
        raise RuntimeError(f"Unknown asset ID: {asset_id}. Not found in PINNED_HASHES.")

    api_url = POLYHAVEN_API_BASE.format(asset_id=asset_id)
    print(f"--> Querying Poly Haven API for {asset_id} ({api_url})...")
    req = urllib.request.Request(
        api_url,
        headers={"User-Agent": "Rime-Asset-Fetcher/1.0 (+https://github.com/rime-engine)"},
    )
    try:
        with urllib.request.urlopen(req) as resp:
            if resp.status != 200:
                raise RuntimeError(f"Poly Haven API returned HTTP {resp.status} for {asset_id}")
            meta = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        raise RuntimeError(f"Failed to query Poly Haven API for {asset_id}: {e}") from e

    blend_catalog = meta.get("blend", {}).get(RESOLUTION, {}).get("blend")
    if not blend_catalog:
        raise RuntimeError(f"Asset {asset_id} has no {RESOLUTION} blend entry in Poly Haven API response")

    blend_url = blend_catalog.get("url")
    if not blend_url:
        raise RuntimeError(f"Asset {asset_id} has no blend url in {RESOLUTION} entry")

    asset_cache_dir = cache_dir / asset_id
    blend_rel = f"{asset_id}_{RESOLUTION}.blend"
    blend_dest = asset_cache_dir / blend_rel

    expected_hashes = PINNED_HASHES[asset_id]
    if blend_rel not in expected_hashes:
        raise RuntimeError(f"Blend file {blend_rel} not found in PINNED_HASHES[{asset_id}]")

    print(f"--> Verifying source files for {asset_id} ({RESOLUTION}):")
    fetch_and_verify_file(blend_url, blend_dest, expected_hashes[blend_rel])

    include_map = blend_catalog.get("include", {})
    for rel_path, tex_info in include_map.items():
        if rel_path not in expected_hashes:
            raise RuntimeError(
                f"Unexpected texture file '{rel_path}' returned by API for {asset_id} "
                f"not present in PINNED_HASHES table"
            )
        tex_url = tex_info.get("url")
        if not tex_url:
            raise RuntimeError(f"No url for texture {rel_path} in {asset_id}")
        tex_dest = asset_cache_dir / rel_path
        fetch_and_verify_file(tex_url, tex_dest, expected_hashes[rel_path])

    # Ensure all pinned files were accounted for
    seen = {blend_rel} | set(include_map.keys())
    for pinned_rel in expected_hashes:
        if pinned_rel not in seen:
            raise RuntimeError(
                f"Pinned file '{pinned_rel}' was not provided by Poly Haven API for {asset_id}"
            )

    return blend_dest


def export_glb_via_blender(
    asset_id: str,
    blend_file: Path,
    out_glb: Path,
    export_script: Path,
) -> None:
    """Run headless Blender with export_gltf.py to produce the output GLB.

    Uses `--python-exit-code 1` so Blender exits non-zero if Python exceptions occur.
    Writes to a temporary `.tmp.glb` file first so failures leave no corrupted file behind.
    (Note: Blender glTF exporter appends `.glb` unless the path ends with `.glb`).
    """
    out_glb.parent.mkdir(parents=True, exist_ok=True)
    temp_glb = out_glb.with_name(f".{out_glb.stem}.tmp.glb")
    if temp_glb.exists():
        temp_glb.unlink()

    blender_bin = os.environ.get("BLENDER", "blender")
    cmd = [
        blender_bin,
        "-b",
        str(blend_file),
        "--python-exit-code",
        "1",
        "--python",
        str(export_script),
        "--",
        str(temp_glb),
    ]

    print(f"--> Exporting {asset_id}.glb via Blender:")
    print(f"    {' '.join(cmd)}")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            if temp_glb.exists():
                temp_glb.unlink()
            sys.stderr.write(proc.stdout)
            sys.stderr.write(proc.stderr)
            raise RuntimeError(f"Blender export for {asset_id} failed with exit code {proc.returncode}")
        if not temp_glb.is_file() or temp_glb.stat().st_size == 0:
            if temp_glb.exists():
                temp_glb.unlink()
            raise RuntimeError(f"Blender completed with exit code 0 but {temp_glb.name} was not produced")
        temp_glb.replace(out_glb)
    except Exception:
        if temp_glb.exists():
            temp_glb.unlink()
        raise

    glb_hash = sha256_of_file(out_glb)
    print(f"--> Successfully produced {out_glb.name}: {out_glb.stat().st_size:,} bytes, SHA-256: {glb_hash}")


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent.parent
    default_cache = repo_root / "build" / "asset-cache"
    default_out = repo_root / "assets" / "src" / "props"
    export_script = repo_root / "assets" / "tools" / "export_gltf.py"

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "assets",
        nargs="*",
        default=list(PINNED_HASHES.keys()),
        help=f"Asset IDs to fetch and export (default: {', '.join(PINNED_HASHES.keys())})",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=default_cache,
        help=f"Directory for caching downloaded assets (default: {default_cache})",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=default_out,
        help=f"Directory for generated GLB files (default: {default_out})",
    )
    args = parser.parse_args()

    if not export_script.is_file():
        raise RuntimeError(f"Blender export script not found at {export_script}")

    for asset_id in args.assets:
        if asset_id not in PINNED_HASHES:
            raise RuntimeError(f"Invalid asset ID '{asset_id}'. Choose from: {list(PINNED_HASHES.keys())}")

    print(f"Fetching {len(args.assets)} prop asset(s) at {RESOLUTION} resolution...")
    for asset_id in args.assets:
        print(f"\n=== Processing {asset_id} ===")
        blend_file = fetch_asset_files(asset_id, args.cache_dir)
        out_glb = args.out_dir / f"{asset_id}.glb"
        export_glb_via_blender(asset_id, blend_file, out_glb, export_script)

    print("\nAll assets fetched, verified, and exported successfully.")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        sys.stderr.write(f"\nfetch_props.py: ERROR: {exc}\n")
        sys.exit(1)
