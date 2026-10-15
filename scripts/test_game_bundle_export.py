# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Exercise publication/refusal behavior independently of a particular build host's ELF.

The real dependency checker and repository-hidden executable run are export-proof.sh's job.
Here injected failures ensure a damaged new input cannot destroy an existing good bundle.
"""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import game_bundle_manifest as manifest
import stage_game_bundle as exporter


class BundleExport(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "source"
        self.repo.mkdir()
        self.binary = self.repo / "hello_game"
        self.binary.write_bytes(b"fixture game bytes\n")
        self.binary.chmod(0o755)
        self.content = self.repo / "content"
        self.content.mkdir()
        self.content.joinpath("hello-game.rscene").write_text("fixture scene\n")
        self.write_provenance()
        notices = {"profile": "hello-game-linux-vulkan-v1"}
        for component, reference in exporter.REFERENCES.items():
            folder = self.repo / component
            folder.mkdir()
            notices["notice_" + component] = str(folder)
            notices["reference_" + component] = reference
            for name in exporter.REQUIRED_TEXTS[component]:
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("nonempty test notice; authentic texts are tested by real export\n")
        for key in ["notice_rime", "notice_rime_notice"]:
            path = self.repo / key
            path.write_text("fixture first-party attribution\n")
            notices[key] = str(path)
        protocols = self.repo / "generated"
        protocols.mkdir()
        for name in exporter.PROTOCOLS:
            protocols.joinpath(name + "-protocol.c").write_text(
                "/* Copyright test fixture. Permission is hereby granted. THE SOFTWARE IS PROVIDED. */\n")
        notices["wayland_generated"] = str(protocols)
        self.notice_record = self.repo / "game.notices"
        self.notice_record.write_text("".join(f"{k}={v}\n" for k, v in notices.items()))
        self.record = self.repo / "game.export"
        fields = {
            "schema": "2", "target": "hello_game", "executable": str(self.binary),
            "content": str(self.content), "configuration": "Debug", "system": "Linux",
            "architecture": "x86_64", "project_version": "0.0.1",
            "bundle_metadata": str(self.notice_record),
        }
        self.record.write_text("".join(f"{k}={v}\n" for k, v in fields.items()))
        self.destination = self.root / "export with spaces"
        patch = mock.patch.object(exporter, "requirements", return_value={"os": "Linux fixture"})
        self.inspection = patch.start()
        self.addCleanup(patch.stop)

    def write_provenance(self):
        self.binary.with_name("hello_game.provenance.json").write_text(json.dumps({
            "schema_version": 1, "source_revision": "1" * 40, "source_dirty": False,
            "binary_sha256": hashlib.sha256(self.binary.read_bytes()).hexdigest(),
        }))

    def publish(self):
        return exporter.stage(self.record, self.destination, self.repo)

    def test_same_bytes_republish_identically_and_keep_build_time_revision(self):
        bundle, digest = self.publish()
        result = manifest.verify(bundle, digest)
        self.assertEqual(result["source_revision"], "1" * 40)
        self.assertEqual(result["network_protocol"], "not-networked")
        self.assertEqual(result["supported_modes"], ["dedicated", "play"])
        _, repeated = self.publish()
        self.assertEqual(digest, repeated)
        self.assertFalse(list(self.destination.glob(".hello_game.*")))

    def test_stale_binary_provenance_preserves_previous_export(self):
        bundle, digest = self.publish()
        self.binary.write_bytes(b"new bytes without matching build stamp")
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.publish()
        manifest.verify(bundle, digest)

    def test_binary_changed_during_inspection_is_not_published(self):
        bundle, digest = self.publish()
        def concurrent_change(*_):
            self.binary.write_bytes(b"changed after initial provenance validation")
            return {"os": "Linux fixture"}
        self.inspection.side_effect = concurrent_change
        with self.assertRaisesRegex(ValueError, "changed while"):
            self.publish()
        manifest.verify(bundle, digest)

    def test_missing_notice_preserves_existing_artifact(self):
        bundle, digest = self.publish()
        self.repo.joinpath("fmt/LICENSE").unlink()
        with self.assertRaises(OSError):
            self.publish()
        manifest.verify(bundle, digest)
        self.assertFalse(list(self.destination.glob(".hello_game.*")))

    def test_content_symlink_refused_without_reading_external_payload(self):
        bundle, digest = self.publish()
        outside = self.root / "private.txt"
        outside.write_text("must not be incorporated")
        self.content.joinpath("link").symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.publish()
        manifest.verify(bundle, digest)
        self.assertEqual(outside.read_text(), "must not be incorporated")

    def test_refuses_destination_symlink_and_repository_destination(self):
        self.destination.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        self.destination.joinpath("hello_game").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "destination"):
            self.publish()
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            exporter.stage(self.record, self.repo / "exports", self.repo)
        self.assertEqual(list(outside.iterdir()), [])

    def test_failed_publish_restores_old_bundle(self):
        bundle, digest = self.publish()
        original_replace = exporter.os.replace
        def fail_publication(source, destination, *args, **kwargs):
            if Path(source).name.startswith(".hello_game.stage-"):
                raise OSError("injected publish failure")
            return original_replace(source, destination, *args, **kwargs)
        with mock.patch.object(exporter.os, "replace", side_effect=fail_publication):
            with self.assertRaisesRegex(OSError, "injected"):
                self.publish()
        manifest.verify(bundle, digest)
        self.assertFalse(list(self.destination.glob(".hello_game.*")))

    def test_failed_rollback_retains_the_old_backup(self):
        _, digest = self.publish()
        original_replace = exporter.os.replace
        def fail_publication_and_rollback(source, destination, *args, **kwargs):
            if Path(source).name.startswith(".hello_game.stage-") or Path(source).name == "payload":
                raise OSError("injected publish/rollback failure")
            return original_replace(source, destination, *args, **kwargs)
        with mock.patch.object(exporter.os, "replace", side_effect=fail_publication_and_rollback):
            with self.assertRaises(OSError):
                self.publish()
        retained = list(self.destination.glob(".hello_game.previous-*/payload"))
        self.assertEqual(len(retained), 1)
        manifest.verify(retained[0], digest)


if __name__ == "__main__":
    unittest.main()
