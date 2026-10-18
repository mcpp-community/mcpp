#!/usr/bin/env python3
"""Package notification contracts; no real dispatches or credentials."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/release/notify_packages.py"
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("notify_packages", SCRIPT)
notify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notify)
sys.path.pop(0)

VERSION = "2026.10.17.1"
TAG = f"v{VERSION}"
COMMIT = "a" * 40


def manifest():
    return {
        "schema": 1, "tag": TAG, "version": VERSION, "commit": COMMIT,
        "assets": [{"name": name, "sha256": "b" * 64}
                   for name in sorted(notify.required_primary_assets(VERSION))],
    }


class Resolve(unittest.TestCase):
    def test_exact_manifest_identity_wins_over_checkout_version(self):
        # The release may rebuild an old tag from a workflow started on main.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "mcpp.toml").write_text('[package]\nversion = "2099.1.1.1"\n')
            (path / "manifest.json").write_text(json.dumps(manifest()))
            env = dict(os.environ, GITHUB_OUTPUT=str(path / "output"))
            result = subprocess.run([
                sys.executable, str(SCRIPT), "resolve", "--manifest", str(path / "manifest.json"),
            ], cwd=path, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"tag={TAG}\n", (path / "output").read_text())
            self.assertIn(f"commit={COMMIT}\n", result.stdout)
            self.assertNotIn("2099", result.stdout)

    def test_all_channels_and_single_channel_retry(self):
        values = notify.resolve(manifest(), "", "all")
        self.assertEqual([row["channel"] for row in json.loads(values["matrix"])["include"]],
                         ["homebrew", "nix"])
        values = notify.resolve(manifest(), TAG, "nix")
        rows = json.loads(values["matrix"])["include"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["channel"], "nix")

    def test_rejects_wrong_requested_tag(self):
        with self.assertRaises(notify.ManifestError):
            notify.resolve(manifest(), "v2026.10.16.3", "all")

    def test_rejects_invalid_identity_and_schema(self):
        for key, value in [("schema", 2), ("schema", True), ("version", "1\nx=y"),
                           ("version", "2026.10.17.1-rc1"), ("tag", "vOTHER"),
                           ("commit", "short"), ("commit", None)]:
            with self.subTest(key=key, value=value):
                data = manifest()
                data[key] = value
                with self.assertRaises(notify.ManifestError):
                    notify.resolve(data, "", "all")

    def test_rejects_incomplete_or_corrupt_assets(self):
        for kind in ("missing", "duplicate", "digest", "type"):
            with self.subTest(kind=kind):
                data = manifest()
                if kind == "missing":
                    data["assets"].pop()
                elif kind == "duplicate":
                    data["assets"].append(copy.deepcopy(data["assets"][0]))
                elif kind == "digest":
                    data["assets"][0]["sha256"] = "invalid"
                else:
                    data["assets"] = None
                with self.assertRaises(notify.ManifestError):
                    notify.resolve(data, "", "all")

    def test_malformed_manifest_cli_fails_without_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "manifest.json").write_text("[]")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "resolve", "--manifest", str(path / "manifest.json"),
            ], env=dict(os.environ, GITHUB_OUTPUT=str(path / "output")), capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((path / "output").exists())


class Dispatch(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.summary = Path(self.directory.name) / "summary"
        self.environment = patch.dict(os.environ, {
            "GH_TOKEN": "fake-token", "GITHUB_STEP_SUMMARY": str(self.summary),
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_each_channel_gets_the_exact_identity(self):
        for channel, repository in [("homebrew", "mcpp-community/homebrew-mcpp"),
                                    ("nix", "mcpp-community/nix-mcpp")]:
            with self.subTest(channel=channel), patch.object(notify.subprocess, "run") as run:
                run.return_value.returncode = 0
                self.assertEqual(notify.notify(channel, TAG, VERSION, COMMIT), 0)
                args, kwargs = run.call_args
                self.assertIn(f"repos/{repository}/dispatches", args[0])
                payload = json.loads(kwargs["input"])
                self.assertEqual(payload["event_type"], "mcpp-release")
                self.assertEqual(payload["client_payload"]["version"], VERSION)
                self.assertEqual(payload["client_payload"]["tag"], TAG)
                self.assertEqual(payload["client_payload"]["commit"], COMMIT)
                self.assertIn(f"/{TAG}/mcpp-release.json", payload["client_payload"]["manifest_url"])
                self.assertNotIn("fake-token", str(run.call_args))
        self.assertIn("Notification accepted", self.summary.read_text())
        self.assertIn("downstream workflow", self.summary.read_text())

    def test_missing_token_skips_without_calling_github(self):
        with patch.dict(os.environ, {"GH_TOKEN": ""}), patch.object(notify.subprocess, "run") as run:
            self.assertEqual(notify.notify("nix", TAG, VERSION, COMMIT), 0)
            run.assert_not_called()
        self.assertIn("Skipped", self.summary.read_text())
        self.assertIn("NIX_PUBLISH_TOKEN", self.summary.read_text())

    def test_dispatch_failure_is_reported_and_does_not_block_other_channel(self):
        with patch.object(notify.subprocess, "run") as run:
            run.return_value.returncode = 1
            self.assertEqual(notify.notify("homebrew", TAG, VERSION, COMMIT), 1)
            run.return_value.returncode = 0
            self.assertEqual(notify.notify("nix", TAG, VERSION, COMMIT), 0)
        summary = self.summary.read_text()
        self.assertIn("Failed to notify homebrew", summary)
        self.assertIn("Notification accepted by nix", summary)

    def test_invalid_identity_never_dispatches(self):
        with patch.object(notify.subprocess, "run") as run:
            with self.assertRaises(notify.ManifestError):
                notify.notify("nix", TAG, "1\nINJECTED=value", COMMIT)
            run.assert_not_called()


class WorkflowContract(unittest.TestCase):
    def test_consumer_uses_completed_run_artifact_instead_of_head_version(self):
        producer = (ROOT / ".github/workflows/release.yml").read_text()
        consumer = (ROOT / ".github/workflows/publish-packages.yml").read_text()
        self.assertIn("name: mcpp-release-manifest", producer)
        self.assertIn("name: mcpp-release-manifest", consumer)
        self.assertIn("run-id: ${{ github.event.workflow_run.id }}", consumer)
        self.assertNotIn("workflow_run.head_sha", consumer)
        self.assertNotIn("mcpp.toml", consumer)
        self.assertIn("fail-fast: false", consumer)
        self.assertFalse((ROOT / ".github/workflows/homebrew-publish.yml").exists())


if __name__ == "__main__":
    unittest.main()
