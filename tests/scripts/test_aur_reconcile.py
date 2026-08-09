#!/usr/bin/env python3
"""State-machine tests for the mcpp-bin-only AUR reconciler."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
AUR_DIR = REPO_ROOT / "scripts" / "aur"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


render = load_module("render_mcpp_bin", AUR_DIR / "render_mcpp_bin.py")
reconcile = load_module("reconcile_mcpp_bin", AUR_DIR / "reconcile_mcpp_bin.py")

VERSION = "2026.8.10.1"
TAG = f"v{VERSION}"
COMMIT = "c" * 40


def version_compare(left: str, right: str) -> int:
    def key(value: str) -> tuple[tuple[int, ...], int]:
        version, release = value.rsplit("-", 1)
        return tuple(int(part) for part in version.split(".")), int(release)

    return (key(left) > key(right)) - (key(left) < key(right))


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.assets = root / "assets"
        self.assets.mkdir()
        self.manifest_path = root / "mcpp-release.json"
        self.rows: list[dict[str, str]] = []
        self.add_asset("linux", "x86_64")
        self.add_asset("linux", "aarch64")
        self.add_asset("macos", "arm64", token="macosx")
        self.write_manifest()

    def add_asset(self, platform: str, arch: str, *, token: str | None = None) -> None:
        token = token or platform
        suffix = "zip" if platform == "windows" else "tar.gz"
        name = f"mcpp-{VERSION}-{token}-{arch}.{suffix}"
        payload = f"real bytes for {name}\n".encode()
        digest = hashlib.sha256(payload).hexdigest()
        (self.assets / name).write_bytes(payload)
        (self.assets / f"{name}.sha256").write_text(
            f"{digest}  {name}\n", encoding="utf-8"
        )
        self.rows.append(
            {"platform": platform, "arch": arch, "name": name, "sha256": digest}
        )

    def write_manifest(self) -> None:
        self.manifest_path.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "version": VERSION,
                    "tag": TAG,
                    "commit": COMMIT,
                    "assets": self.rows,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )


class FakeRunner:
    def __init__(self, failure=None) -> None:
        self.failure = failure
        self.calls: list[tuple[str, ...]] = []

    def run(self, args, **_kwargs):
        self.calls.append(tuple(str(arg) for arg in args))
        if self.failure is not None:
            raise self.failure
        return reconcile.CommandResult(stdout="", stderr="", returncode=0)


class AurReconcileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name)
        self.fixture = Fixture(self.root)
        self.desired = render.load_desired_state(self.fixture.manifest_path)

    def observed(
        self,
        *,
        rpc: str,
        git: str,
        content_matches: bool,
    ):
        return reconcile.ObservedState(
            rpc_version=rpc,
            git_version=git,
            git_head="d" * 40,
            content_matches=content_matches,
        )

    def test_renderer_consumes_manifest_and_materializes_only_mcpp_bin(self) -> None:
        protected = AUR_DIR / "mcpp-m"
        before = tree_digest(protected)
        output = self.root / "rendered"

        render.materialize_package(
            desired=self.desired,
            template_dir=AUR_DIR / "mcpp-bin",
            output_dir=output,
        )

        pkgbuild = (output / "PKGBUILD").read_text(encoding="utf-8")
        self.assertIn(f"pkgver={VERSION}", pkgbuild)
        self.assertIn(
            "# Maintainer: mcpp-community <speak-agent@users.noreply.github.com>",
            pkgbuild,
        )
        self.assertIn(self.desired.asset("linux", "x86_64").sha256, pkgbuild)
        self.assertIn(self.desired.asset("linux", "aarch64").sha256, pkgbuild)
        self.assertEqual(
            (output / "mcpp.sh").read_bytes(),
            (AUR_DIR / "mcpp-bin" / "mcpp.sh").read_bytes(),
        )
        self.assertFalse((output / ".SRCINFO").exists())
        self.assertEqual(before, tree_digest(protected))

    def test_both_linux_payloads_and_sidecars_are_recomputed(self) -> None:
        hashes = reconcile.validate_release_assets(self.desired, self.fixture.assets)
        self.assertEqual(set(hashes), {"x86_64", "aarch64"})
        for arch, digest in hashes.items():
            self.assertEqual(digest, self.desired.asset("linux", arch).sha256)

    def test_missing_and_hash_mismatched_asset_are_permanent(self) -> None:
        x86 = self.desired.asset("linux", "x86_64")
        (self.fixture.assets / f"{x86.name}.sha256").unlink()
        with self.assertRaises(reconcile.ReconcileFailure) as missing:
            reconcile.validate_release_assets(self.desired, self.fixture.assets)
        self.assertEqual(missing.exception.classification.value, "permanent")

        (self.fixture.assets / f"{x86.name}.sha256").write_text(
            f"{'0' * 64}  {x86.name}\n", encoding="utf-8"
        )
        with self.assertRaises(reconcile.ReconcileFailure) as mismatch:
            reconcile.validate_release_assets(self.desired, self.fixture.assets)
        self.assertEqual(mismatch.exception.classification.value, "permanent")

    def test_noop_upgrade_repair_wait_and_late_event_refusal_plans(self) -> None:
        desired = self.desired.package_version
        rows = (
            (self.observed(rpc=desired, git=desired, content_matches=True), "noop"),
            (
                self.observed(
                    rpc="2026.8.1.1-1", git="2026.8.1.1-1", content_matches=False
                ),
                "upgrade",
            ),
            (self.observed(rpc=desired, git=desired, content_matches=False), "repair"),
            (
                self.observed(rpc="2026.8.1.1-1", git=desired, content_matches=True),
                "wait-rpc",
            ),
            (
                self.observed(
                    rpc="2026.9.1.1-1", git="2026.9.1.1-1", content_matches=True
                ),
                "refuse-downgrade",
            ),
        )
        for observed, action in rows:
            with self.subTest(action=action):
                plan = reconcile.plan_reconciliation(desired, observed, version_compare)
                self.assertEqual(plan.action.value, action)

    def test_maintenance_is_retried_with_bounded_backoff(self) -> None:
        attempts = 0
        sleeps: list[float] = []

        def operation():
            nonlocal attempts
            attempts += 1
            if attempts < 3:
                raise reconcile.CommandFailure(
                    ("git", "push"), 1, "", "AUR is down for maintenance"
                )
            return "pushed"

        value, retries = reconcile.retry_transient(
            operation,
            max_attempts=4,
            base_delay=2,
            sleep=sleeps.append,
        )
        self.assertEqual(value, "pushed")
        self.assertEqual(retries, 2)
        self.assertEqual(sleeps, [2, 4])

    def test_auth_failure_is_permanent_and_not_retried(self) -> None:
        attempts = 0

        def operation():
            nonlocal attempts
            attempts += 1
            raise reconcile.CommandFailure(
                ("git", "push"), 128, "", "Permission denied (publickey)"
            )

        with self.assertRaises(reconcile.ReconcileFailure) as failure:
            reconcile.retry_transient(
                operation,
                max_attempts=4,
                base_delay=1,
                sleep=lambda _delay: None,
            )
        self.assertEqual(failure.exception.classification.value, "permanent")
        self.assertEqual(attempts, 1)

    def test_rpc_lag_after_git_update_is_polled_without_another_push(self) -> None:
        values = iter(("2026.8.1.1-1", "2026.8.1.1-1", self.desired.package_version))
        sleeps: list[float] = []
        version, polls = reconcile.poll_rpc_version(
            lambda: next(values),
            desired_version=self.desired.package_version,
            compare=version_compare,
            max_attempts=4,
            delay=3,
            sleep=sleeps.append,
        )
        self.assertEqual(version, self.desired.package_version)
        self.assertEqual(polls, 3)
        self.assertEqual(sleeps, [3, 3])

    def test_known_package_clone_failure_never_initializes_first_publish(self) -> None:
        runner = FakeRunner(
            reconcile.CommandFailure(
                ("git", "clone"), 1, "", "503 Service Unavailable: maintenance"
            )
        )
        destination = self.root / "aur-clone"
        with self.assertRaises(reconcile.ReconcileFailure) as failure:
            reconcile.clone_observed_repo(runner, destination)
        self.assertEqual(failure.exception.classification.value, "transient")
        self.assertFalse((destination / ".git").exists())
        self.assertTrue(runner.calls)
        self.assertFalse(any("init" in call for call in runner.calls))

    def test_only_latest_complete_stable_tag_can_be_selected(self) -> None:
        releases = [
            {
                "tag_name": "v2026.8.10.2",
                "draft": False,
                "prerelease": False,
                "assets": [{"name": "still-uploading.tar.gz"}],
            },
            {
                "tag_name": TAG,
                "draft": False,
                "prerelease": False,
                "assets": [{"name": "mcpp-release.json"}],
            },
            {
                "tag_name": "v2026.8.8.9",
                "draft": False,
                "prerelease": False,
                "assets": [{"name": "mcpp-release.json"}],
            },
        ]
        self.assertEqual(reconcile.select_complete_release(releases)["tag_name"], TAG)
        with self.assertRaises(reconcile.ReconcileFailure) as old:
            reconcile.select_complete_release(releases, requested_tag="v2026.8.8.9")
        self.assertEqual(old.exception.classification.value, "refused-downgrade")

    def test_executable_automation_has_no_mcpp_m_path_or_publish_leg(self) -> None:
        paths = (
            AUR_DIR / "render_mcpp_bin.py",
            AUR_DIR / "reconcile_mcpp_bin.py",
            AUR_DIR / "update.sh",
            REPO_ROOT / ".github" / "workflows" / "aur-publish.yml",
        )
        for path in paths:
            with self.subTest(path=path):
                text = path.read_text(encoding="utf-8")
                self.assertNotIn("scripts/aur/mcpp-m", text)
                self.assertNotIn("publish mcpp-m", text)
                self.assertNotIn("publish(mcpp-m", text)

    def test_publish_uses_public_speak_agent_identity(self) -> None:
        self.assertEqual(reconcile.AUR_COMMIT_NAME, "speak-agent")
        self.assertEqual(
            reconcile.AUR_COMMIT_EMAIL,
            "speak-agent@users.noreply.github.com",
        )

    def _publish_decision(
        self,
        trigger: str,
        autopublish: str = "",
        manual: str = "",
    ) -> str:
        """Run the workflow's own publish-decision shell, not a paraphrase of it."""
        workflow = (
            REPO_ROOT / ".github" / "workflows" / "aur-publish.yml"
        ).read_text(encoding="utf-8")
        match = re.search(r'^\s*case "\$TRIGGER" in$.*?^\s*esac$',
                          workflow, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(match, "publish decision block not found")
        # The block also emits a `::notice::` line, so mark the value rather
        # than reading whatever happens to be on stdout.
        script = f'{match.group(0)}\nprintf "<publish>%s</publish>" "$publish"'
        result = subprocess.run(
            ["bash", "-c", script],
            check=False,
            capture_output=True,
            text=True,
            env={
                "PATH": os.environ.get("PATH", ""),
                "TRIGGER": trigger,
                "AUTOPUBLISH": autopublish,
                "MANUAL_PUBLISH": manual,
            },
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        value = re.search(r"<publish>(.*?)</publish>", result.stdout)
        self.assertIsNotNone(value, result.stdout)
        return value.group(1)

    def test_automatic_triggers_do_not_publish_until_armed(self) -> None:
        # Merging a workflow is a decision about code. Pushing to the AUR is a
        # decision about the outside world. `schedule` runs every six hours off
        # the default branch, so without this gate the two are the same act:
        # merge, wait six hours, and mcpp has written to a third-party service
        # with nobody watching — on a path that had never completed a real push.
        for trigger in ("workflow_run", "schedule"):
            with self.subTest(trigger=trigger, armed=False):
                self.assertEqual(self._publish_decision(trigger), "false")
            with self.subTest(trigger=trigger, armed=True):
                self.assertEqual(
                    self._publish_decision(trigger, autopublish="true"), "true")
            # Anything other than an exact "true" leaves it disarmed, so a typo
            # in the repository variable fails closed.
            with self.subTest(trigger=trigger, armed="typo"):
                self.assertEqual(
                    self._publish_decision(trigger, autopublish="yes"), "false")

        # Manual dispatch keeps its explicit per-run switch: that is how the
        # first, watched publish is meant to happen.
        self.assertEqual(self._publish_decision("workflow_dispatch"), "false")
        self.assertEqual(
            self._publish_decision("workflow_dispatch", manual="true"), "true")
        # …and arming the automatic triggers must not silently arm dispatch.
        self.assertEqual(
            self._publish_decision("workflow_dispatch", autopublish="true"),
            "false")

    def test_workflow_uses_pinned_host_key_and_recovery_triggers(self) -> None:
        workflow = (
            REPO_ROOT / ".github" / "workflows" / "aur-publish.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("workflow_run:", workflow)
        self.assertIn("schedule:", workflow)
        self.assertIn("workflow_dispatch:", workflow)
        self.assertNotIn("ssh-keyscan", workflow)
        self.assertNotIn("--force", workflow)

        known_hosts = AUR_DIR / "aur.archlinux.org.known_hosts"
        result = subprocess.run(
            ["ssh-keygen", "-lf", str(known_hosts), "-E", "sha256"],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "SHA256:RFzBCUItH9LZS0cKB5UE6ceAYhBD5C8GeOBip8Z11+4",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
