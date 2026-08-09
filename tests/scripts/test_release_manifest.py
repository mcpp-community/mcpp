#!/usr/bin/env python3
"""Contract tests for the immutable GitHub release manifest generator."""

from __future__ import annotations

import hashlib
import json
import random
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "release" / "generate_manifest.py"
VERSION = "2026.8.10.1"
TAG = f"v{VERSION}"
COMMIT = "a" * 40
PRIMARY_ASSETS = (
    f"mcpp-{VERSION}-linux-x86_64.tar.gz",
    f"mcpp-{VERSION}-linux-aarch64.tar.gz",
    f"mcpp-{VERSION}-macosx-arm64.tar.gz",
    f"mcpp-{VERSION}-windows-x86_64.zip",
)


class ReleaseFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.assets_dir = root / "assets"
        self.assets_dir.mkdir()
        self.release_json = root / "release.json"
        self.output = root / "mcpp-release.json"
        self.release = {
            "tag_name": TAG,
            "draft": False,
            "prerelease": False,
            "assets": [],
        }
        for name in PRIMARY_ASSETS:
            self.add_primary(name)
        self.write_release()

    def add_primary(self, name: str, payload: bytes | None = None) -> str:
        payload = payload if payload is not None else f"payload:{name}\n".encode()
        (self.assets_dir / name).write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        sidecar = f"{name}.sha256"
        (self.assets_dir / sidecar).write_text(
            f"{digest}  {name}\n", encoding="utf-8"
        )
        self.release["assets"].extend(({"name": name}, {"name": sidecar}))
        return digest

    def write_release(self) -> None:
        self.release_json.write_text(
            json.dumps(self.release, indent=2) + "\n", encoding="utf-8"
        )

    def run(
        self,
        *,
        version: str = VERSION,
        tag: str = TAG,
        commit: str = COMMIT,
        output: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "--release-json",
                str(self.release_json),
                "--assets-dir",
                str(self.assets_dir),
                "--version",
                version,
                "--tag",
                tag,
                "--commit",
                commit,
                "--output",
                str(output or self.output),
            ],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )


class ReleaseManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.fixture = ReleaseFixture(Path(self.tempdir.name))

    def assert_failed(self, result: subprocess.CompletedProcess[str], text: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(text, result.stderr.lower())

    def test_complete_manifest_is_deterministic_and_sorted(self) -> None:
        first = self.fixture.run()
        self.assertEqual(first.returncode, 0, first.stderr)
        first_bytes = self.fixture.output.read_bytes()

        random.Random(398).shuffle(self.fixture.release["assets"])
        self.fixture.write_release()
        second_output = self.fixture.root / "second.json"
        second = self.fixture.run(output=second_output)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first_bytes, second_output.read_bytes())

        manifest = json.loads(first_bytes)
        self.assertEqual(manifest["schema"], 1)
        self.assertEqual(manifest["version"], VERSION)
        self.assertEqual(manifest["tag"], TAG)
        self.assertEqual(manifest["commit"], COMMIT)
        self.assertEqual(
            [(item["platform"], item["arch"]) for item in manifest["assets"]],
            [
                ("linux", "aarch64"),
                ("linux", "x86_64"),
                ("macos", "arm64"),
                ("windows", "x86_64"),
            ],
        )
        for item in manifest["assets"]:
            payload = self.fixture.assets_dir / item["name"]
            self.assertEqual(item["sha256"], hashlib.sha256(payload.read_bytes()).hexdigest())

    def test_duplicate_normalized_platform_arch_is_rejected(self) -> None:
        self.fixture.add_primary(f"mcpp-{VERSION}-macos-arm64.tar.gz")
        self.fixture.write_release()
        self.assert_failed(self.fixture.run(), "duplicate platform/arch")

    def test_every_additional_versioned_platform_asset_is_included(self) -> None:
        name = f"mcpp-{VERSION}-freebsd-riscv64.tar.gz"
        digest = self.fixture.add_primary(name)
        self.fixture.write_release()
        result = self.fixture.run()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(self.fixture.output.read_text(encoding="utf-8"))
        self.assertIn(
            {
                "platform": "freebsd",
                "arch": "riscv64",
                "name": name,
                "sha256": digest,
            },
            manifest["assets"],
        )

    def test_missing_sidecar_is_rejected(self) -> None:
        primary = PRIMARY_ASSETS[0]
        sidecar = f"{primary}.sha256"
        (self.fixture.assets_dir / sidecar).unlink()
        self.fixture.release["assets"] = [
            asset for asset in self.fixture.release["assets"] if asset["name"] != sidecar
        ]
        self.fixture.write_release()
        self.assert_failed(self.fixture.run(), "missing sidecar")

    def test_mismatched_sidecar_hash_is_rejected(self) -> None:
        primary = PRIMARY_ASSETS[1]
        (self.fixture.assets_dir / f"{primary}.sha256").write_text(
            f"{'0' * 64}  {primary}\n", encoding="utf-8"
        )
        self.assert_failed(self.fixture.run(), "sha256 mismatch")

    def test_draft_and_prerelease_are_rejected(self) -> None:
        for field in ("draft", "prerelease"):
            with self.subTest(field=field):
                self.fixture.release[field] = True
                self.fixture.write_release()
                self.assert_failed(self.fixture.run(), field)
                self.fixture.release[field] = False

    def test_wrong_release_tag_is_rejected(self) -> None:
        self.fixture.release["tag_name"] = "v2099.1.1"
        self.fixture.write_release()
        self.assert_failed(self.fixture.run(), "release tag")

    def test_tag_must_equal_v_prefixed_version(self) -> None:
        self.assert_failed(self.fixture.run(tag="vreally-wrong"), "tag/version")

    def test_asset_version_must_match_manifest_version(self) -> None:
        new_version = "2026.8.10.2"
        self.fixture.release["tag_name"] = f"v{new_version}"
        self.fixture.write_release()
        self.assert_failed(
            self.fixture.run(version=new_version, tag=f"v{new_version}"),
            "asset version",
        )

    def test_missing_required_primary_asset_is_rejected(self) -> None:
        primary = PRIMARY_ASSETS[-1]
        names = {primary, f"{primary}.sha256"}
        for name in names:
            (self.fixture.assets_dir / name).unlink()
        self.fixture.release["assets"] = [
            asset for asset in self.fixture.release["assets"] if asset["name"] not in names
        ]
        self.fixture.write_release()
        self.assert_failed(self.fixture.run(), "missing required primary asset")

    def test_duplicate_release_asset_names_are_rejected(self) -> None:
        self.fixture.release["assets"].append({"name": PRIMARY_ASSETS[0]})
        self.fixture.write_release()
        self.assert_failed(self.fixture.run(), "duplicate release asset name")

    def test_commit_must_be_a_full_object_id(self) -> None:
        self.assert_failed(self.fixture.run(commit="abc123"), "commit")


if __name__ == "__main__":
    unittest.main()
