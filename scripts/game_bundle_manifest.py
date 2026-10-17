# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Create and check a deterministic manifest for a staged Linux game bundle.

The manifest describes every regular payload file, including notices and documentation.
An externally trusted manifest SHA-256 pins those exact bytes and detects replacement of
both manifest and payload. An unsigned manifest alone does NOT authenticate its publisher:
anyone able to rewrite the payload can also calculate and write new matching hashes.

Python 3.11+, standard library only. Neither API executes the game or removes payload.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import tomllib
import unicodedata

MANIFEST_NAME = "rime-game.toml"
_METADATA_KEYS = frozenset({
    "game_id", "target_triple", "configuration", "executable", "source_revision",
    "source_dirty", "network_protocol", "supported_modes", "requirements", "required_notices",
})
_MANIFEST_KEYS = _METADATA_KEYS | {"schema_version", "content_sha256", "payload"}
_RECORD_KEYS = frozenset({"path", "size", "sha256", "executable"})
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_REVISION = re.compile(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})\Z")


class ManifestError(ValueError):
    """A bundle or its declared manifest does not satisfy the shipping contract."""


def _string(value: object, name: str) -> str:
    if type(value) is not str or not value or not value.strip():
        raise ManifestError(f"{name} must be a nonempty string")
    if any(unicodedata.category(char) in {"Cc", "Cs"} for char in value):
        raise ManifestError(f"{name} contains a control character or invalid Unicode scalar")
    return value


def _relative_path(value: object, name: str = "path") -> str:
    value = _string(value, name)
    if (value.startswith("/") or "\\" in value or ":" in value
            or any(part in {"", ".", ".."} for part in value.split("/"))
            or unicodedata.normalize("NFC", value) != value):
        raise ManifestError(f"{name} must be a normalized relative POSIX path: {value!r}")
    return value


def _keys(value: Mapping, expected: frozenset | set, name: str) -> None:
    if set(value) != expected:
        missing, unknown = expected - set(value), set(value) - expected
        raise ManifestError(f"{name} keys differ (missing={sorted(missing)!r}, "
                            f"unknown={sorted(unknown, key=str)!r})")


def _metadata(value: object) -> dict:
    if not isinstance(value, Mapping):
        raise ManifestError("metadata must be a mapping")
    _keys(value, _METADATA_KEYS, "metadata")
    result = {key: _string(value[key], key) for key in (
        "game_id", "target_triple", "configuration", "source_revision", "network_protocol")}
    if not _REVISION.fullmatch(result["source_revision"]):
        raise ManifestError("source_revision must contain exactly 40 or 64 hexadecimal digits")
    if type(value["source_dirty"]) is not bool:
        raise ManifestError("source_dirty must be a boolean")
    result["source_dirty"] = value["source_dirty"]
    result["executable"] = _relative_path(value["executable"], "executable")
    if result["executable"] == MANIFEST_NAME:
        raise ManifestError("the manifest cannot be the executable")
    modes = value["supported_modes"]
    if (type(modes) is not list or not modes or any(type(mode) is not str for mode in modes)
            or any(mode not in {"play", "dedicated"} for mode in modes)
            or len(set(modes)) != len(modes)):
        raise ManifestError("supported_modes must be a nonempty unique list of play/dedicated")
    result["supported_modes"] = sorted(modes)
    requirements = value["requirements"]
    if not isinstance(requirements, Mapping) or not requirements:
        raise ManifestError("requirements must be a nonempty string mapping of measured ABI needs")
    result["requirements"] = {}
    for key, item in requirements.items():
        result["requirements"][_string(key, "requirement key")] = _string(item, "requirement value")
    notices = value["required_notices"]
    if type(notices) is not list or not notices:
        raise ManifestError("required_notices must be a nonempty list of relative paths")
    notices = [_relative_path(path, "required notice") for path in notices]
    if len(set(notices)) != len(notices) or MANIFEST_NAME in notices:
        raise ManifestError("required_notices must be unique payload paths")
    result["required_notices"] = sorted(notices)
    return result


def _open_bundle(bundle: str | os.PathLike) -> int:
    path = Path(bundle)
    # Opening each component with O_NOFOLLOW avoids resolving a symlink before traversal
    # even begins. Child directories and files use the same descriptor-relative rule.
    absolute = Path(os.path.abspath(path))
    fd = os.open(absolute.anchor, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for component in absolute.parts[1:]:
            child = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                            dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def _manifest_kind(root_fd: int, *, required: bool = False) -> None:
    try:
        info = os.stat(MANIFEST_NAME, dir_fd=root_fd, follow_symlinks=False)
    except FileNotFoundError:
        if required:
            raise ManifestError(f"missing {MANIFEST_NAME}") from None
        return
    if not stat.S_ISREG(info.st_mode):
        raise ManifestError(f"{MANIFEST_NAME} must be a regular file, never a link or directory")


def _read_regular(
    parent_fd: int, name: str, relative: str, *, retain: bool = False
) -> tuple[bytes, str, os.stat_result]:
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent_fd)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ManifestError(f"not a regular file: {relative!r}")
        chunks, digest = [], hashlib.sha256()
        while chunk := os.read(fd, 1024 * 1024):
            digest.update(chunk)
            if retain:
                chunks.append(chunk)
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns, before.st_mode) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_mode):
            raise ManifestError(f"file changed while reading: {relative!r}")
        return b"".join(chunks), digest.hexdigest(), after
    finally:
        os.close(fd)


def _scan(root_fd: int) -> tuple[list[dict], set[str]]:
    records, directories = [], set()

    def walk(directory_fd: int, prefix: str) -> None:
        with os.scandir(directory_fd) as entries:
            for entry in entries:
                relative = _relative_path(prefix + entry.name)
                info = entry.stat(follow_symlinks=False)
                if stat.S_ISLNK(info.st_mode):
                    raise ManifestError(f"symlinks are forbidden: {relative!r}")
                if stat.S_ISDIR(info.st_mode):
                    if relative == MANIFEST_NAME:
                        raise ManifestError(f"{MANIFEST_NAME} cannot be a directory")
                    directories.add(relative)
                    child_fd = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                       dir_fd=directory_fd)
                    try:
                        walk(child_fd, relative + "/")
                    finally:
                        os.close(child_fd)
                elif stat.S_ISREG(info.st_mode):
                    if relative == MANIFEST_NAME:
                        continue
                    _, digest, actual = _read_regular(directory_fd, entry.name, relative)
                    records.append({"path": relative, "size": actual.st_size,
                                    "sha256": digest,
                                    "executable": bool(actual.st_mode & 0o111)})
                else:
                    raise ManifestError(f"special files are forbidden: {relative!r}")

    walk(root_fd, "")
    records.sort(key=lambda record: record["path"])
    return records, directories


def _content_sha256(records: list[dict]) -> str:
    content = [{key: record[key] for key in ("path", "size", "sha256")}
               for record in records if record["path"].startswith("content/")]
    # Canonical JSON is only the unambiguous hash framing, not another shipped format.
    # Including the relative names and sizes prevents moving bytes between files from
    # preserving a content identity. Execute flags remain in the full payload manifest.
    framed = json.dumps(content, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(framed).hexdigest()


def _required_payload(metadata: dict, records: list[dict], directories: set[str]) -> None:
    by_path = {record["path"]: record for record in records}
    content = [record for record in records if record["path"].startswith("content/")]
    if "content" not in directories or not content or not any(record["size"] for record in content):
        raise ManifestError("content/ must contain a nonempty regular payload file")
    executable = by_path.get(metadata["executable"])
    if executable is None or executable["size"] == 0 or not executable["executable"]:
        raise ManifestError("declared executable must exist, be nonempty, and have an execute permission bit")
    for notice in metadata["required_notices"]:
        if notice not in by_path or by_path[notice]["size"] == 0:
            raise ManifestError(f"required notice is missing or empty: {notice!r}")


def _quoted(value: str) -> str:
    # JSON basic-string escapes are also TOML escapes; retain real Unicode scalars so
    # supplementary-plane characters do not become invalid TOML surrogate escapes.
    return json.dumps(value, ensure_ascii=False)


def _toml(metadata: dict, records: list[dict]) -> bytes:
    lines = ["schema_version = 1"]
    for key in sorted(_METADATA_KEYS - {"requirements"}):
        value = metadata[key]
        if type(value) is bool:
            encoded = str(value).lower()
        elif type(value) is list:
            encoded = "[" + ", ".join(_quoted(item) for item in value) + "]"
        else:
            encoded = _quoted(value)
        lines.append(f"{key} = {encoded}")
    lines += [f'content_sha256 = "{_content_sha256(records)}"', "", "[requirements]"]
    for key, value in sorted(metadata["requirements"].items()):
        lines.append(f"{_quoted(key)} = {_quoted(value)}")
    for record in records:
        lines += ["", "[[payload]]", f'path = {_quoted(record["path"])}',
                  f'size = {record["size"]}', f'sha256 = "{record["sha256"]}"',
                  f'executable = {str(record["executable"]).lower()}']
    return ("\n".join(lines) + "\n").encode("utf-8")


def _atomic_manifest(root_fd: int, data: bytes) -> None:
    _manifest_kind(root_fd)
    temporary = f".rime-game-{secrets.token_hex(16)}.tmp"
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                 0o644, dir_fd=root_fd)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        _manifest_kind(root_fd)
        os.replace(temporary, MANIFEST_NAME, src_dir_fd=root_fd, dst_dir_fd=root_fd)
        os.fsync(root_fd)
    finally:
        # This name was created exclusively above. Cleanup only our own temporary file;
        # payload files are never removed, even when validation or replacement fails.
        try:
            os.unlink(temporary, dir_fd=root_fd)
        except FileNotFoundError:
            pass


def create(bundle: str | os.PathLike, metadata: Mapping) -> str:
    """Atomically write rime-game.toml and return its SHA-256; never edit payload files."""
    metadata = _metadata(metadata)
    root_fd = _open_bundle(bundle)
    try:
        _manifest_kind(root_fd)
        records, directories = _scan(root_fd)
        _required_payload(metadata, records, directories)
        data = _toml(metadata, records)
        _atomic_manifest(root_fd, data)
        return hashlib.sha256(data).hexdigest()
    finally:
        os.close(root_fd)


def _declared_manifest(data: bytes) -> tuple[dict, list[dict], str]:
    try:
        parsed = tomllib.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ManifestError(f"invalid manifest TOML: {error}") from error
    _keys(parsed, _MANIFEST_KEYS, "manifest")
    if type(parsed["schema_version"]) is not int or parsed["schema_version"] != 1:
        raise ManifestError("schema_version must be integer 1")
    _metadata({key: parsed[key] for key in _METADATA_KEYS})
    if type(parsed["content_sha256"]) is not str or not _SHA256.fullmatch(parsed["content_sha256"]):
        raise ManifestError("content_sha256 must be a lowercase SHA-256 digest")
    records = parsed["payload"]
    if type(records) is not list or not records:
        raise ManifestError("payload must be a nonempty list of file records")
    previous = None
    for record in records:
        if type(record) is not dict:
            raise ManifestError("payload records must be tables")
        _keys(record, _RECORD_KEYS, "payload record")
        relative = _relative_path(record["path"], "payload path")
        if relative == MANIFEST_NAME:
            raise ManifestError("the manifest cannot describe itself as payload")
        if previous is not None and relative <= previous:
            raise ManifestError("payload paths must be sorted and unique")
        previous = relative
        if type(record["size"]) is not int or record["size"] < 0:
            raise ManifestError("payload size must be a nonnegative integer")
        if type(record["sha256"]) is not str or not _SHA256.fullmatch(record["sha256"]):
            raise ManifestError("payload sha256 must be a lowercase SHA-256 digest")
        if type(record["executable"]) is not bool:
            raise ManifestError("payload executable must be a boolean")
    return parsed, records, parsed["content_sha256"]


def verify(bundle: str | os.PathLike, expected_manifest_sha256: str | None = None) -> dict:
    """Check the exact staged tree and return its manifest; optionally pin trusted bytes.

    Without an externally trusted digest this checks consistency, not publisher authenticity.
    """
    if expected_manifest_sha256 is not None:
        if (type(expected_manifest_sha256) is not str
                or not re.fullmatch(r"[0-9a-fA-F]{64}", expected_manifest_sha256)):
            raise ManifestError("expected manifest SHA-256 must contain 64 hexadecimal digits")
        expected_manifest_sha256 = expected_manifest_sha256.lower()
    root_fd = _open_bundle(bundle)
    try:
        _manifest_kind(root_fd, required=True)
        data, actual_digest, _ = _read_regular(root_fd, MANIFEST_NAME, MANIFEST_NAME, retain=True)
        if (expected_manifest_sha256 is not None
                and not hmac.compare_digest(actual_digest, expected_manifest_sha256)):
            raise ManifestError("manifest SHA-256 does not match the trusted expected digest")
        parsed, declared, content_digest = _declared_manifest(data)
        actual, directories = _scan(root_fd)
        _required_payload(parsed, actual, directories)
        if declared != actual:
            raise ManifestError("payload differs from manifest (paths, size, hash, or execute bit)")
        if not hmac.compare_digest(content_digest, _content_sha256(actual)):
            raise ManifestError("content_sha256 does not match content/ payload")
        return parsed
    finally:
        os.close(root_fd)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create_parser = commands.add_parser("create", help="write an exact staged-bundle manifest")
    create_parser.add_argument("bundle", type=Path)
    create_parser.add_argument("--metadata", required=True, help="inline JSON metadata object")
    verify_parser = commands.add_parser("verify", help="verify manifest and exact staged payload")
    verify_parser.add_argument("bundle", type=Path)
    verify_parser.add_argument("--manifest-sha256", help="externally trusted manifest digest")
    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            metadata = json.loads(args.metadata)
            digest = create(args.bundle, metadata)
        else:
            # Select and pin this invocation's bytes before verification so the printed
            # identity is exactly the manifest checked, even if another writer replaces it.
            root_fd = _open_bundle(args.bundle)
            try:
                _manifest_kind(root_fd, required=True)
                _, digest, _ = _read_regular(root_fd, MANIFEST_NAME, MANIFEST_NAME)
            finally:
                os.close(root_fd)
            verify(args.bundle, digest if args.manifest_sha256 is None else args.manifest_sha256)
            if (args.manifest_sha256 is not None
                    and not hmac.compare_digest(digest, args.manifest_sha256.lower())):
                raise ManifestError("manifest changed during verification")
        print(digest)
        return 0
    except (ManifestError, OSError, json.JSONDecodeError) as error:
        print(f"game bundle manifest: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
