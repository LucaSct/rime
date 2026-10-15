# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Run: python3 -m unittest discover -s scripts -p test_game_bundle_manifest.py -v"""

import copy
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import tempfile
import unittest
from unittest.mock import patch

import game_bundle_manifest as manifest


class GameBundleManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.bundle = Path(self.temporary.name) / "game"
        self.bundle.mkdir()
        self.write("bin/hello-game", b"ELF test fixture executable\n", 0o755)
        self.write("content/scene.rscene", b"authored hello-game scene\n")
        self.write("README.md", b"Run bin/hello-game.\n")
        self.write("LICENSES/NOTICE.txt", b"Engine and bundled-content notices.\n")
        self.metadata = {
            "game_id": "hello-game", "target_triple": "x86_64-unknown-linux-gnu",
            "configuration": "Release", "executable": "bin/hello-game",
            "source_revision": "a" * 40, "source_dirty": False,
            "network_protocol": "not-networked", "supported_modes": ["play", "dedicated"],
            "requirements": {"GLIBC": "2.43", "architecture": "x86_64"},
            "required_notices": ["LICENSES/NOTICE.txt"],
        }

    @property
    def manifest_path(self):
        return self.bundle / manifest.MANIFEST_NAME

    def write(self, name, data, mode=0o644):
        path = self.bundle / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        return path

    def create(self):
        return manifest.create(self.bundle, self.metadata)

    def tamper(self, old, new):
        text = self.manifest_path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        self.manifest_path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def assert_rejected(self, operation):
        with self.assertRaises((manifest.ManifestError, OSError)):
            operation()

    def test_roundtrip_exact_payload_and_determinism(self):
        original_metadata = copy.deepcopy(self.metadata)
        digest = self.create()
        first = self.manifest_path.read_bytes()
        parsed = manifest.verify(self.bundle, digest)
        self.assertEqual(hashlib.sha256(first).hexdigest(), digest)
        self.assertEqual(self.metadata, original_metadata)
        self.assertEqual(parsed["supported_modes"], ["dedicated", "play"])
        paths = [record["path"] for record in parsed["payload"]]
        self.assertEqual(paths, ["LICENSES/NOTICE.txt", "README.md", "bin/hello-game", "content/scene.rscene"])
        self.assertEqual(parsed["requirements"]["GLIBC"], "2.43")
        self.assertEqual(digest, self.create())
        self.assertEqual(first, self.manifest_path.read_bytes())
        # Tree enumeration order, metadata insertion order, mtimes and modes unrelated to
        # execution must not leak the host or staging session into release identity.
        self.metadata = dict(reversed(list(self.metadata.items())))
        self.metadata["requirements"] = dict(reversed(list(self.metadata["requirements"].items())))
        for path in self.bundle.rglob("*"):
            if path.is_file():
                os.utime(path, (1234, 1234))
        (self.bundle / "README.md").chmod(0o600)
        self.assertEqual(digest, self.create())
        self.assertEqual(first, self.manifest_path.read_bytes())
        self.assertEqual([manifest.MANIFEST_NAME], [p.name for p in self.bundle.iterdir() if p.name.endswith(".toml")])
        self.assertFalse(any(p.name.startswith(".rime-game-") for p in self.bundle.iterdir()))

    def test_determinism_across_separately_staged_trees(self):
        digest = self.create()
        other = Path(self.temporary.name) / "separate"
        other.mkdir()
        for relative in ["content/scene.rscene", "README.md", "LICENSES/NOTICE.txt", "bin/hello-game"]:
            destination = other / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            source = self.bundle / relative
            destination.write_bytes(source.read_bytes())
            destination.chmod(source.stat().st_mode)
        self.assertEqual(digest, manifest.create(other, self.metadata))
        self.assertEqual(self.manifest_path.read_bytes(), (other / manifest.MANIFEST_NAME).read_bytes())

    def test_content_identity_tracks_names_bytes_and_sizes_only(self):
        self.create()
        initial = manifest.verify(self.bundle)["content_sha256"]
        (self.bundle / "README.md").write_bytes(b"new documentation")
        self.create()
        self.assertEqual(initial, manifest.verify(self.bundle)["content_sha256"])
        (self.bundle / "content/scene.rscene").rename(self.bundle / "content/renamed.rscene")
        self.create()
        self.assertNotEqual(initial, manifest.verify(self.bundle)["content_sha256"])

    def test_mutated_executable_or_content_is_detected_including_same_size(self):
        for relative in ("bin/hello-game", "content/scene.rscene", "README.md", "LICENSES/NOTICE.txt"):
            for change_size in (False, True):
                with self.subTest(path=relative, change_size=change_size):
                    original = (self.bundle / relative).read_bytes()
                    self.create()
                    replacement = b"x" * len(original) + (b"!" if change_size else b"")
                    (self.bundle / relative).write_bytes(replacement)
                    self.assert_rejected(lambda: manifest.verify(self.bundle))
                    (self.bundle / relative).write_bytes(original)

    def test_required_content_and_notice_cannot_be_deleted_or_emptied(self):
        for relative in ("content/scene.rscene", "LICENSES/NOTICE.txt"):
            for empty in (False, True):
                with self.subTest(path=relative, empty=empty):
                    path = self.bundle / relative
                    original = path.read_bytes()
                    self.create()
                    path.write_bytes(b"") if empty else path.unlink()
                    self.assert_rejected(lambda: manifest.verify(self.bundle))
                    self.assert_rejected(self.create)
                    path.write_bytes(original)

    def test_missing_content_directory_is_rejected(self):
        (self.bundle / "content/scene.rscene").unlink()
        (self.bundle / "content").rmdir()
        self.assert_rejected(self.create)

    def test_executable_must_exist_be_nonempty_and_have_execute_bit(self):
        executable = self.bundle / "bin/hello-game"
        original = executable.read_bytes()
        self.create()
        executable.chmod(0o644)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.assert_rejected(self.create)
        executable.chmod(0o755)
        executable.write_bytes(b"")
        self.assert_rejected(self.create)
        executable.unlink()
        self.assert_rejected(self.create)
        executable.write_bytes(original)

    def test_execute_bit_changes_for_any_payload_file_are_detected(self):
        self.create()
        (self.bundle / "content/scene.rscene").chmod(0o755)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.create()
        manifest.verify(self.bundle)
        (self.bundle / "bin/hello-game").chmod(0o700)
        # Owner/group/other distinctions are intentionally not part of the boolean record.
        manifest.verify(self.bundle)

    def test_added_payload_is_detected_and_next_creation_includes_it(self):
        self.create()
        self.write("content/new.bin", b"new payload")
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.create()
        self.assertIn("content/new.bin", [record["path"] for record in manifest.verify(self.bundle)["payload"]])

    def test_unsafe_actual_payload_names_are_rejected(self):
        for name in ("bad\\name", "bad:name", "bad\nname", "bad\x7fname", "e\u0301.bin"):
            with self.subTest(name=name):
                path = self.write(name, b"not canonical")
                self.assert_rejected(self.create)
                path.unlink()

    def test_unsafe_manifest_paths_are_rejected_without_opening_them(self):
        for unsafe in ("/etc/passwd", "../escape", "content/../escape", "content//bad",
                       "./README.md", "content/./bad", "content/", "C:/bad", "bad\\name",
                       "bad\nname", "e\u0301.bin"):
            for field in ("payload", "executable", "notice"):
                with self.subTest(path=unsafe, field=field):
                    self.create()
                    old = {"payload": 'path = "README.md"', "executable": 'executable = "bin/hello-game"',
                           "notice": 'required_notices = ["LICENSES/NOTICE.txt"]'}[field]
                    new = {"payload": "path = " + json.dumps(unsafe),
                           "executable": "executable = " + json.dumps(unsafe),
                           "notice": "required_notices = [" + json.dumps(unsafe) + "]"}[field]
                    self.tamper(old, new)
                    self.assert_rejected(lambda: manifest.verify(self.bundle))

    def test_all_symlink_kinds_are_rejected_by_both_apis(self):
        outside = Path(self.temporary.name) / "outside"
        outside.mkdir()
        (outside / "file").write_bytes(b"outside unchanged")
        self.create()
        for name, target in (("link-file", outside / "file"), ("link-dir", outside),
                             ("dangling", outside / "absent"), ("loop", self.bundle)):
            with self.subTest(name=name):
                link = self.bundle / name
                link.symlink_to(target)
                self.assert_rejected(self.create)
                self.assert_rejected(lambda: manifest.verify(self.bundle))
                link.unlink()
        self.manifest_path.unlink()
        self.manifest_path.symlink_to(outside / "file")
        self.assert_rejected(self.create)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.assertEqual((outside / "file").read_bytes(), b"outside unchanged")

    def test_bundle_or_ancestor_symlinks_are_rejected(self):
        self.create()
        link = Path(self.temporary.name) / "linked-game"
        link.symlink_to(self.bundle, target_is_directory=True)
        self.assert_rejected(lambda: manifest.verify(link))
        self.assert_rejected(lambda: manifest.create(link, self.metadata))
        parent_link = Path(self.temporary.name) / "linked-parent"
        parent_link.symlink_to(Path(self.temporary.name), target_is_directory=True)
        self.assert_rejected(lambda: manifest.verify(parent_link / "game"))

    def test_manifest_directory_and_special_payload_are_rejected(self):
        self.manifest_path.mkdir()
        self.assert_rejected(self.create)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.manifest_path.rmdir()
        os.mkfifo(self.manifest_path)
        self.assert_rejected(self.create)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.manifest_path.unlink()
        self.create()
        fifo = self.bundle / "pipe"
        os.mkfifo(fifo)
        self.assert_rejected(self.create)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        fifo.unlink()
        unix_socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(unix_socket.close)
        unix_socket.bind(str(self.bundle / "socket"))
        self.assert_rejected(self.create)
        self.assert_rejected(lambda: manifest.verify(self.bundle))

    def test_duplicate_payload_records_are_rejected(self):
        self.create()
        text = self.manifest_path.read_text()
        first = text.split("[[payload]]", 2)[1]
        self.manifest_path.write_text(text + "\n[[payload]]" + first)
        self.assert_rejected(lambda: manifest.verify(self.bundle))

    def test_unknown_keys_schema_and_exact_types_are_rejected(self):
        mutations = [
            ("schema_version = 1", "schema_version = true"),
            ("schema_version = 1", "schema_version = 2"),
            ("schema_version = 1", "schema_version = 1\nunknown = 1"),
            ("source_dirty = false", 'source_dirty = "false"'),
            ("source_revision = " + json.dumps("a" * 40), 'source_revision = "HEAD"'),
            ('supported_modes = ["dedicated", "play"]', 'supported_modes = ["editor"]'),
            ('supported_modes = ["dedicated", "play"]', 'supported_modes = ["play", "play"]'),
            ('required_notices = ["LICENSES/NOTICE.txt"]', 'required_notices = []'),
            ('size = ' + str((self.bundle / 'LICENSES/NOTICE.txt').stat().st_size), 'size = true'),
            ('executable = false', 'executable = 0'),
        ]
        for old, new in mutations:
            with self.subTest(new=new):
                self.create()
                self.tamper(old, new)
                self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.create()
        self.manifest_path.write_text(self.manifest_path.read_text() + "unexpected = 1\n")
        self.assert_rejected(lambda: manifest.verify(self.bundle))

    def test_invalid_toml_utf8_and_duplicate_keys_are_rejected(self):
        for data in (b"\xff", b"schema_version = 1\nschema_version = 1\n", b"not TOML"):
            with self.subTest(data=data):
                self.create()
                self.manifest_path.write_bytes(data)
                self.assert_rejected(lambda: manifest.verify(self.bundle))

    def test_identical_file_bytes_remain_distinct_payload_paths(self):
        self.write("content/duplicate.rscene", (self.bundle / "content/scene.rscene").read_bytes())
        self.create()
        content = [record for record in manifest.verify(self.bundle)["payload"]
                   if record["path"].startswith("content/")]
        self.assertEqual(len(content), 2)
        self.assertEqual(content[0]["sha256"], content[1]["sha256"])
        self.assertNotEqual(content[0]["path"], content[1]["path"])

    def test_malformed_metadata_is_rejected_before_writing(self):
        cases = [
            {"source_dirty": 0}, {"source_revision": "f" * 39}, {"source_revision": "g" * 64},
            {"supported_modes": []}, {"supported_modes": [True]}, {"supported_modes": "play"},
            {"requirements": {}}, {"requirements": {"GLIBC": 2.43}},
            {"required_notices": []}, {"required_notices": ["LICENSES/NOTICE.txt"] * 2},
            {"required_notices": [manifest.MANIFEST_NAME]}, {"game_id": ""}, {"game_id": "bad\x00"},
            {"executable": "../outside"}, {"executable": manifest.MANIFEST_NAME},
            {"unknown": "field"},
        ]
        for update in cases:
            with self.subTest(update=update):
                metadata = {**self.metadata, **update}
                self.assert_rejected(lambda: manifest.create(self.bundle, metadata))
                self.assertFalse(self.manifest_path.exists())
        del self.metadata["requirements"]
        self.assert_rejected(self.create)

    def test_sha256_revisions_and_unicode_metadata_roundtrip(self):
        self.metadata["source_revision"] = "ABCDEF01" * 8
        self.metadata["game_id"] = "game-\U0001f3ae"
        self.write("content/\u00e9.txt", b"normalized Unicode path")
        digest = self.create()
        self.assertEqual(manifest.verify(self.bundle, digest)["game_id"], "game-\U0001f3ae")

    def test_rewrite_payload_and_manifest_fails_original_trusted_digest(self):
        original = self.create()
        self.write("content/scene.rscene", b"changed game, internally consistent hashes")
        rewritten = self.create()
        self.assertNotEqual(original, rewritten)
        manifest.verify(self.bundle)  # Consistency alone is not publisher authentication.
        manifest.verify(self.bundle, rewritten.upper())
        self.assert_rejected(lambda: manifest.verify(self.bundle, original))
        self.metadata["supported_modes"] = ["dedicated"]
        self.create()
        self.assert_rejected(lambda: manifest.verify(self.bundle, rewritten))

    def test_bad_content_digest_and_missing_manifest_are_rejected(self):
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        self.create()
        actual = manifest.verify(self.bundle)["content_sha256"]
        self.tamper(actual, "0" * 64)
        self.assert_rejected(lambda: manifest.verify(self.bundle))
        for expected in ("short", 123, True, "g" * 64):
            self.assert_rejected(lambda: manifest.verify(self.bundle, expected))

    def test_failed_atomic_write_preserves_existing_manifest_and_payload(self):
        self.create()
        before = {p.relative_to(self.bundle).as_posix(): p.read_bytes()
                  for p in self.bundle.rglob("*") if p.is_file()}
        with patch.object(manifest.os, "replace", side_effect=OSError("simulated replacement failure")):
            self.assert_rejected(self.create)
        after = {p.relative_to(self.bundle).as_posix(): p.read_bytes()
                 for p in self.bundle.rglob("*") if p.is_file()}
        self.assertEqual(before, after)

    def test_cli_create_verify_and_errors_without_launching_game(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(manifest.main(["create", str(self.bundle), "--metadata", json.dumps(self.metadata)]), 0)
        digest = stdout.getvalue().strip()
        self.assertEqual(digest, hashlib.sha256(self.manifest_path.read_bytes()).hexdigest())
        stdout = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(manifest.main(["verify", str(self.bundle), "--manifest-sha256", digest]), 0)
        self.assertEqual(stdout.getvalue().strip(), digest)
        with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            self.assertEqual(manifest.main(["verify", str(self.bundle), "--manifest-sha256", "0" * 64]), 1)
            self.assertEqual(manifest.main(["create", str(self.bundle), "--metadata", "not json"]), 1)
            self.assertEqual(manifest.main(["verify", str(self.bundle), "--manifest-sha256", ""]), 1)
        self.assertIn("trusted expected digest", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
