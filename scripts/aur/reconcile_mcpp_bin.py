#!/usr/bin/env python3
"""Reconcile AUR mcpp-bin to the latest complete stable mcpp release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Any, Callable, NoReturn, TypeVar

from render_mcpp_bin import (
    DesiredState,
    RenderError,
    load_desired_state,
    materialize_package,
)


GITHUB_REPOSITORY = "mcpp-community/mcpp"
AUR_PACKAGE = "mcpp-bin"
AUR_HTTPS_URL = "https://aur.archlinux.org/mcpp-bin.git"
AUR_SSH_URL = "ssh://aur@aur.archlinux.org/mcpp-bin.git"
AUR_RPC_URL = "https://aur.archlinux.org/rpc/v5/info?arg[]=mcpp-bin"
ARCH_IMAGE = "archlinux:base-devel"
MANAGED_FILES = ("PKGBUILD", ".SRCINFO", "mcpp.sh")
AUR_COMMIT_NAME = "speak-agent"
AUR_COMMIT_EMAIL = "speak-agent@users.noreply.github.com"
SIDECAR_RE = re.compile(r"^([0-9a-fA-F]{64})[ \t]+\*?(\S+)$")
T = TypeVar("T")


class Classification(str, Enum):
    NOOP = "noop"
    UPDATED = "updated"
    TRANSIENT = "transient"
    PERMANENT = "permanent"
    REFUSED_DOWNGRADE = "refused-downgrade"


class PlanAction(str, Enum):
    NOOP = "noop"
    UPGRADE = "upgrade"
    REPAIR = "repair"
    WAIT_RPC = "wait-rpc"
    REFUSE_DOWNGRADE = "refuse-downgrade"


class ReconcileFailure(RuntimeError):
    def __init__(
        self,
        message: str,
        classification: Classification,
        *,
        retryable: bool = False,
    ) -> None:
        super().__init__(message)
        self.classification = classification
        self.retryable = retryable


class CommandFailure(RuntimeError):
    def __init__(
        self,
        command: tuple[str, ...] | list[str],
        returncode: int,
        stdout: str,
        stderr: str,
    ) -> None:
        self.command = tuple(command)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(
            f"command failed ({returncode}): {' '.join(self.command)}\n{stderr.strip()}"
        )


@dataclass(frozen=True)
class CommandResult:
    stdout: str
    stderr: str
    returncode: int


class CommandRunner:
    def run(
        self,
        args: list[str] | tuple[str, ...],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        timeout: float | None = None,
    ) -> CommandResult:
        command = tuple(str(arg) for arg in args)
        try:
            completed = subprocess.run(
                command,
                cwd=cwd,
                env=env,
                timeout=timeout,
                check=False,
                capture_output=True,
                text=True,
            )
        except subprocess.TimeoutExpired as exc:
            raise CommandFailure(
                command,
                124,
                exc.stdout or "",
                f"command timed out after {timeout}s: {exc.stderr or ''}",
            ) from exc
        except OSError as exc:
            raise CommandFailure(command, 127, "", str(exc)) from exc
        result = CommandResult(completed.stdout, completed.stderr, completed.returncode)
        if completed.returncode != 0:
            raise CommandFailure(
                command,
                result.returncode,
                result.stdout,
                result.stderr,
            )
        return result


@dataclass(frozen=True)
class ObservedState:
    rpc_version: str | None
    git_version: str
    git_head: str
    content_matches: bool


@dataclass(frozen=True)
class ReconcilePlan:
    action: PlanAction
    reason: str


def permanent(message: str) -> NoReturn:
    raise ReconcileFailure(message, Classification.PERMANENT)


def classify_command_failure(error: CommandFailure) -> ReconcileFailure:
    detail = f"{error.stdout}\n{error.stderr}".lower()
    auth_markers = (
        "permission denied",
        "publickey",
        "authentication failed",
        "host key verification failed",
        "invalid format",
    )
    transient_markers = (
        "maintenance",
        "temporarily unavailable",
        "service unavailable",
        "timed out",
        "timeout",
        "connection reset",
        "connection closed",
        "remote end hung up",
        "ssh_exchange_identification",
        "http 502",
        "http 503",
        "http 504",
        "502 bad gateway",
        "503 service unavailable",
        "504 gateway timeout",
    )
    if any(marker in detail for marker in auth_markers):
        return ReconcileFailure(str(error), Classification.PERMANENT)
    if "non-fast-forward" in detail or "fetch first" in detail:
        return ReconcileFailure(
            str(error), Classification.TRANSIENT, retryable=False
        )
    if any(marker in detail for marker in transient_markers):
        return ReconcileFailure(str(error), Classification.TRANSIENT, retryable=True)
    return ReconcileFailure(str(error), Classification.PERMANENT)


def retry_transient(
    operation: Callable[[], T],
    *,
    max_attempts: int,
    base_delay: float,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[T, int]:
    if max_attempts < 1:
        permanent("retry max_attempts must be at least 1")
    retries = 0
    for attempt in range(1, max_attempts + 1):
        try:
            return operation(), retries
        except CommandFailure as error:
            failure = classify_command_failure(error)
        except ReconcileFailure as error:
            failure = error
        if not failure.retryable or attempt == max_attempts:
            raise failure
        delay = base_delay * (2 ** (attempt - 1))
        sleep(delay)
        retries += 1
    raise AssertionError("retry loop must return or raise")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        permanent(f"cannot read release asset {path}: {exc}")
    return digest.hexdigest()


def _sidecar_digest(path: Path, primary_name: str) -> str:
    try:
        lines = [
            line.strip()
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    except (OSError, UnicodeError) as exc:
        permanent(f"missing or unreadable release sidecar {path}: {exc}")
    if len(lines) != 1:
        permanent(f"release sidecar must have one checksum record: {path.name}")
    match = SIDECAR_RE.fullmatch(lines[0])
    if match is None or match.group(2) != primary_name:
        permanent(f"invalid release sidecar record: {path.name}")
    return match.group(1).lower()


def validate_release_assets(
    desired: DesiredState, assets_dir: Path
) -> dict[str, str]:
    validated: dict[str, str] = {}
    for arch in ("x86_64", "aarch64"):
        asset = desired.asset("linux", arch)
        payload = assets_dir / asset.name
        sidecar = assets_dir / f"{asset.name}.sha256"
        if not payload.is_file():
            permanent(f"missing release payload: {asset.name}")
        if not sidecar.is_file():
            permanent(f"missing release sidecar: {sidecar.name}")
        sidecar_digest = _sidecar_digest(sidecar, asset.name)
        actual_digest = _sha256_file(payload)
        if sidecar_digest != asset.sha256:
            permanent(
                f"sidecar/manifest SHA256 mismatch for {asset.name}: "
                f"{sidecar_digest} != {asset.sha256}"
            )
        if actual_digest != asset.sha256:
            permanent(
                f"payload/manifest SHA256 mismatch for {asset.name}: "
                f"{actual_digest} != {asset.sha256}"
            )
        validated[arch] = actual_digest
    return validated


def select_complete_release(
    releases: Any, requested_tag: str | None = None
) -> dict[str, Any]:
    if not isinstance(releases, list):
        permanent("GitHub releases response must be an array")
    selected: dict[str, Any] | None = None
    for release in releases:
        if not isinstance(release, dict):
            permanent("GitHub release row must be an object")
        if release.get("draft") is not False or release.get("prerelease") is not False:
            continue
        assets = release.get("assets")
        if not isinstance(assets, list):
            continue
        names = {
            item.get("name")
            for item in assets
            if isinstance(item, dict) and isinstance(item.get("name"), str)
        }
        if "mcpp-release.json" in names:
            selected = release
            break
    if selected is None:
        raise ReconcileFailure(
            "no complete stable release with mcpp-release.json is available",
            Classification.TRANSIENT,
            retryable=True,
        )
    selected_tag = selected.get("tag_name")
    if not isinstance(selected_tag, str) or not selected_tag:
        permanent("selected GitHub release has no valid tag_name")
    if requested_tag and requested_tag != selected_tag:
        raise ReconcileFailure(
            f"requested tag {requested_tag!r} is not latest complete stable "
            f"tag {selected_tag!r}",
            Classification.REFUSED_DOWNGRADE,
        )
    return selected


def plan_reconciliation(
    desired_version: str,
    observed: ObservedState,
    compare: Callable[[str, str], int],
) -> ReconcilePlan:
    git_comparison = compare(desired_version, observed.git_version)
    rpc_comparison = (
        compare(desired_version, observed.rpc_version)
        if observed.rpc_version is not None
        else 1
    )
    if git_comparison < 0 or rpc_comparison < 0:
        return ReconcilePlan(
            PlanAction.REFUSE_DOWNGRADE,
            "AUR already contains a version newer than desired state",
        )
    if git_comparison == 0 and observed.content_matches:
        if rpc_comparison == 0:
            return ReconcilePlan(PlanAction.NOOP, "git, RPC, and desired content agree")
        return ReconcilePlan(
            PlanAction.WAIT_RPC,
            "AUR git is desired state while RPC metadata is still behind",
        )
    if git_comparison > 0:
        return ReconcilePlan(PlanAction.UPGRADE, "desired version is newer than AUR git")
    return ReconcilePlan(
        PlanAction.REPAIR,
        "AUR version matches desired state but tracked package content differs",
    )


def poll_rpc_version(
    fetch_version: Callable[[], str | None],
    *,
    desired_version: str,
    compare: Callable[[str, str], int],
    max_attempts: int,
    delay: float,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[str, int]:
    if max_attempts < 1:
        permanent("RPC max_attempts must be at least 1")
    last: str | None = None
    for attempt in range(1, max_attempts + 1):
        last = fetch_version()
        if last is not None:
            comparison = compare(desired_version, last)
            if comparison == 0:
                return last, attempt
            if comparison < 0:
                raise ReconcileFailure(
                    f"AUR RPC advanced to {last}, newer than desired {desired_version}",
                    Classification.REFUSED_DOWNGRADE,
                )
        if attempt < max_attempts:
            sleep(delay)
    raise ReconcileFailure(
        f"AUR RPC did not converge to {desired_version}; last={last!r}",
        Classification.TRANSIENT,
        retryable=True,
    )


def clone_observed_repo(
    runner: Any,
    destination: Path,
    url: str = AUR_HTTPS_URL,
) -> None:
    try:
        runner.run(["git", "clone", "--", url, str(destination)])
    except CommandFailure as error:
        raise classify_command_failure(error) from error
    if not (destination / ".git").is_dir():
        permanent(f"known AUR package clone produced no git repository: {destination}")


def _run_or_classify(
    runner: CommandRunner,
    args: list[str],
    *,
    cwd: Path | None = None,
    timeout: float | None = None,
) -> CommandResult:
    try:
        return runner.run(args, cwd=cwd, timeout=timeout)
    except CommandFailure as error:
        raise classify_command_failure(error) from error


def _parse_json(text: str, label: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        lower = text.lower()
        if "maintenance" in lower or "service unavailable" in lower:
            raise ReconcileFailure(
                f"{label} returned transient non-JSON response",
                Classification.TRANSIENT,
                retryable=True,
            ) from exc
        permanent(f"{label} returned invalid JSON: {exc}")


def _release_asset_names(release: dict[str, Any]) -> set[str]:
    raw_assets = release.get("assets")
    if not isinstance(raw_assets, list):
        permanent("selected GitHub release assets must be an array")
    names: set[str] = set()
    for item in raw_assets:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            permanent("selected GitHub release has an invalid asset row")
        name = item["name"]
        if name in names:
            permanent(f"selected GitHub release has duplicate asset name: {name}")
        names.add(name)
    return names


def _download_release_asset(
    runner: CommandRunner,
    *,
    repository: str,
    tag: str,
    name: str,
    destination: Path,
) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    try:
        runner.run(
            [
                "gh",
                "release",
                "download",
                tag,
                "--repo",
                repository,
                "--dir",
                str(destination),
                "--pattern",
                name,
            ],
            timeout=300,
        )
    except CommandFailure as error:
        failure = classify_command_failure(error)
        if failure.classification is Classification.PERMANENT:
            permanent(f"cannot download required release asset {name}: {error}")
        raise failure from error
    path = destination / name
    if not path.is_file():
        permanent(f"GitHub download omitted required release asset: {name}")
    return path


@dataclass(frozen=True)
class DesiredSnapshot:
    desired: DesiredState
    manifest_path: Path
    assets_dir: Path
    release: dict[str, Any] | None


def acquire_desired_snapshot(
    runner: CommandRunner,
    *,
    work_dir: Path,
    repository: str,
    requested_tag: str | None,
    local_manifest: Path | None,
    local_assets_dir: Path | None,
) -> DesiredSnapshot:
    release: dict[str, Any] | None = None
    if local_manifest is not None:
        desired = load_desired_state(local_manifest)
        if requested_tag and requested_tag != desired.tag:
            raise ReconcileFailure(
                f"requested tag {requested_tag!r} does not match local manifest "
                f"tag {desired.tag!r}",
                Classification.REFUSED_DOWNGRADE,
            )
        manifest_path = local_manifest
    else:
        response = _run_or_classify(
            runner,
            ["gh", "api", f"repos/{repository}/releases?per_page=20"],
            timeout=60,
        )
        release = select_complete_release(
            _parse_json(response.stdout, "GitHub releases API"), requested_tag
        )
        tag = release["tag_name"]
        manifest_dir = work_dir / "manifest"
        manifest_path = _download_release_asset(
            runner,
            repository=repository,
            tag=tag,
            name="mcpp-release.json",
            destination=manifest_dir,
        )
        desired = load_desired_state(manifest_path)
        if desired.tag != tag:
            permanent(
                f"release/manifest tag mismatch: release={tag!r}, "
                f"manifest={desired.tag!r}"
            )

    if local_assets_dir is not None:
        assets_dir = local_assets_dir
    else:
        assets_dir = work_dir / "assets"
        release_names = _release_asset_names(release) if release is not None else None
        for arch in ("x86_64", "aarch64"):
            asset = desired.asset("linux", arch)
            for name in (asset.name, f"{asset.name}.sha256"):
                if release_names is not None and name not in release_names:
                    permanent(f"selected GitHub release inventory is missing {name}")
                _download_release_asset(
                    runner,
                    repository=repository,
                    tag=desired.tag,
                    name=name,
                    destination=assets_dir,
                )
    validate_release_assets(desired, assets_dir)
    return DesiredSnapshot(desired, manifest_path, assets_dir, release)


def generate_srcinfo_and_verify(
    runner: CommandRunner,
    *,
    package_dir: Path,
    image: str,
) -> None:
    script = """
set -euo pipefail
useradd --create-home builder
cp -a /input /tmp/package
chown -R builder:builder /tmp/package
runuser -u builder -- env HOME=/home/builder bash -c 'cd /tmp/package && makepkg --printsrcinfo' > /output/SRCINFO
runuser -u builder -- env HOME=/home/builder bash -c 'cd /tmp/package && makepkg --verifysource --noconfirm'
""".strip()
    with tempfile.TemporaryDirectory(prefix="mcpp-aur-arch-output-") as raw_output:
        generated_dir = Path(raw_output)
        _run_or_classify(
            runner,
            [
                "docker",
                "run",
                "--rm",
                "--volume",
                f"{package_dir.resolve()}:/input:ro",
                "--volume",
                f"{generated_dir.resolve()}:/output",
                image,
                "bash",
                "-euc",
                script,
            ],
            timeout=900,
        )
        generated = generated_dir / "SRCINFO"
        if not generated.is_file():
            permanent("Arch makepkg did not generate .SRCINFO")
        try:
            content = generated.read_bytes()
            (package_dir / ".SRCINFO").write_bytes(content)
        except OSError as exc:
            permanent(f"cannot install generated .SRCINFO: {exc}")


def _srcinfo_package_version(path: Path) -> str:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        permanent(f"cannot read AUR .SRCINFO {path}: {exc}")
    version_match = re.search(r"^\s*pkgver\s*=\s*(\S+)\s*$", text, re.MULTILINE)
    release_match = re.search(r"^\s*pkgrel\s*=\s*(\S+)\s*$", text, re.MULTILINE)
    if version_match is None or release_match is None:
        permanent(f"AUR .SRCINFO lacks pkgver/pkgrel: {path}")
    return f"{version_match.group(1)}-{release_match.group(1)}"


class VersionComparator:
    def __init__(self, runner: CommandRunner, image: str) -> None:
        self.runner = runner
        self.image = image
        self.command = shutil.which("vercmp")
        self.cache: dict[tuple[str, str], int] = {}

    def __call__(self, left: str, right: str) -> int:
        key = (left, right)
        if key in self.cache:
            return self.cache[key]
        args = (
            [self.command, left, right]
            if self.command
            else ["docker", "run", "--rm", self.image, "vercmp", left, right]
        )
        result = _run_or_classify(self.runner, args, timeout=120)
        try:
            value = int(result.stdout.strip())
        except ValueError:
            permanent(f"Arch vercmp returned invalid output: {result.stdout!r}")
        value = (value > 0) - (value < 0)
        self.cache[key] = value
        return value


@dataclass(frozen=True)
class RpcState:
    version: str
    last_modified: int | None


def fetch_rpc_state(runner: CommandRunner) -> RpcState:
    result = _run_or_classify(
        runner,
        [
            "curl",
            "--fail",
            "--silent",
            "--show-error",
            "--location",
            "--connect-timeout",
            "15",
            "--max-time",
            "30",
            AUR_RPC_URL,
        ],
        timeout=45,
    )
    payload = _parse_json(result.stdout, "AUR RPC")
    if not isinstance(payload, dict) or not isinstance(payload.get("results"), list):
        permanent("AUR RPC response has no results array")
    rows = payload["results"]
    if len(rows) != 1 or not isinstance(rows[0], dict):
        permanent(
            "known AUR package mcpp-bin is missing or ambiguous; refusing first publish"
        )
    version = rows[0].get("Version")
    modified = rows[0].get("LastModified")
    if not isinstance(version, str) or not version:
        permanent("AUR RPC mcpp-bin row has no valid Version")
    if modified is not None and type(modified) is not int:
        modified = None
    return RpcState(version, modified)


def inspect_observed_repo(
    runner: CommandRunner,
    *,
    clone_dir: Path,
    desired_package_dir: Path,
    rpc_state: RpcState,
) -> ObservedState:
    clone_observed_repo(runner, clone_dir)
    git_version = _srcinfo_package_version(clone_dir / ".SRCINFO")
    git_head = _run_or_classify(
        runner, ["git", "rev-parse", "HEAD"], cwd=clone_dir
    ).stdout.strip()
    if re.fullmatch(r"[0-9a-fA-F]{40,64}", git_head) is None:
        permanent(f"AUR git returned invalid HEAD: {git_head!r}")
    tracked_text = _run_or_classify(
        runner, ["git", "ls-files", "-z"], cwd=clone_dir
    ).stdout
    tracked = {item for item in tracked_text.split("\0") if item}
    content_matches = tracked == set(MANAGED_FILES)
    for name in MANAGED_FILES:
        observed_path = clone_dir / name
        desired_path = desired_package_dir / name
        if not observed_path.is_file() or not desired_path.is_file():
            content_matches = False
            continue
        if observed_path.read_bytes() != desired_path.read_bytes():
            content_matches = False
    return ObservedState(
        rpc_version=rpc_state.version,
        git_version=git_version,
        git_head=git_head.lower(),
        content_matches=content_matches,
    )


def stage_desired_repo(
    runner: CommandRunner, *, clone_dir: Path, desired_package_dir: Path
) -> str:
    tracked_text = _run_or_classify(
        runner, ["git", "ls-files", "-z"], cwd=clone_dir
    ).stdout
    tracked = {item for item in tracked_text.split("\0") if item}
    for name in sorted(tracked - set(MANAGED_FILES)):
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            permanent(f"unsafe tracked path in AUR repository: {name!r}")
        _run_or_classify(runner, ["git", "rm", "-f", "--", name], cwd=clone_dir)
    for name in MANAGED_FILES:
        try:
            shutil.copyfile(desired_package_dir / name, clone_dir / name)
        except OSError as exc:
            permanent(f"cannot stage desired AUR file {name}: {exc}")
    _run_or_classify(
        runner, ["git", "add", "--", *MANAGED_FILES], cwd=clone_dir
    )
    return _run_or_classify(
        runner,
        ["git", "diff", "--cached", "--no-ext-diff", "--"],
        cwd=clone_dir,
    ).stdout


def publish_staged_repo(
    runner: CommandRunner,
    *,
    clone_dir: Path,
    desired_version: str,
    max_attempts: int,
    base_delay: float,
) -> tuple[str, int]:
    _run_or_classify(
        runner,
        ["git", "config", "user.name", AUR_COMMIT_NAME],
        cwd=clone_dir,
    )
    _run_or_classify(
        runner,
        ["git", "config", "user.email", AUR_COMMIT_EMAIL],
        cwd=clone_dir,
    )
    _run_or_classify(
        runner,
        ["git", "commit", "-m", f"mcpp-bin {desired_version}"],
        cwd=clone_dir,
    )
    local_head = _run_or_classify(
        runner, ["git", "rev-parse", "HEAD"], cwd=clone_dir
    ).stdout.strip()

    def push() -> CommandResult:
        return runner.run(
            ["git", "push", AUR_SSH_URL, "HEAD:master"],
            cwd=clone_dir,
            timeout=120,
        )

    _, retries = retry_transient(
        push,
        max_attempts=max_attempts,
        base_delay=base_delay,
    )
    remote = _run_or_classify(
        runner,
        ["git", "ls-remote", AUR_HTTPS_URL, "refs/heads/master"],
        timeout=60,
    ).stdout.split()
    if not remote or remote[0].lower() != local_head.lower():
        raise ReconcileFailure(
            f"AUR public git head does not match pushed commit {local_head}",
            Classification.TRANSIENT,
            retryable=True,
        )
    return local_head.lower(), retries


def clean_arch_install_smoke(
    runner: CommandRunner,
    *,
    package_dir: Path,
    desired_version: str,
    image: str,
) -> None:
    script = """
set -euo pipefail
pacman -Syu --noconfirm --needed git
useradd --create-home builder
cp -a /input /tmp/package
chown -R builder:builder /tmp/package
runuser -u builder -- bash -lc 'cd /tmp/package && makepkg --noconfirm'
pacman -U --noconfirm /tmp/package/mcpp-bin-*.pkg.tar.zst
install -d -o builder -g builder /tmp/mcpp-home
actual=$(runuser -u builder -- env HOME=/home/builder MCPP_HOME=/tmp/mcpp-home mcpp --version)
printf '%s\n' "$actual"
grep -F -- "$EXPECTED_VERSION" <<<"$actual"
""".strip()
    env_args = ["--env", f"EXPECTED_VERSION={desired_version}"]
    _run_or_classify(
        runner,
        [
            "docker",
            "run",
            "--rm",
            *env_args,
            "--volume",
            f"{package_dir.resolve()}:/input:ro",
            image,
            "bash",
            "-euc",
            script,
        ],
        timeout=1800,
    )


def _release_age_seconds(release: dict[str, Any] | None) -> int | None:
    if release is None:
        return None
    value = release.get("published_at")
    if not isinstance(value, str):
        return None
    try:
        published = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return max(0, int((datetime.now(timezone.utc) - published).total_seconds()))


def write_report(report: dict[str, Any], json_path: Path | None, summary: Path | None) -> None:
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if json_path is not None:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(rendered, encoding="utf-8")
    if summary is not None:
        rows = [
            ("trigger", report.get("trigger")),
            ("classification", report.get("classification")),
            ("action", report.get("action")),
            ("desired", report.get("desired_version")),
            ("observed RPC", report.get("observed_rpc_version")),
            ("observed git", report.get("observed_git_version")),
            ("manifest SHA256", report.get("manifest_sha256")),
            ("x86_64 SHA256", report.get("asset_sha256", {}).get("x86_64")),
            ("aarch64 SHA256", report.get("asset_sha256", {}).get("aarch64")),
            ("remote commit", report.get("remote_commit")),
            ("push retries", report.get("push_retries")),
            ("RPC polls", report.get("rpc_polls")),
            ("drift age seconds", report.get("drift_age_seconds")),
            ("published", report.get("published")),
            ("message", report.get("message")),
        ]
        def cell(value: Any) -> str:
            return str(value).replace("|", "\\|").replace("`", "'").replace("\n", " ")

        lines = ["## mcpp-bin AUR reconciliation", "", "| Field | Value |", "|---|---|"]
        lines.extend(f"| {key} | `{cell(value)}` |" for key, value in rows)
        with summary.open("a", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n")


def reconcile_once(
    args: argparse.Namespace,
    *,
    runner: CommandRunner,
    work_dir: Path,
    report: dict[str, Any],
) -> None:
    snapshot = acquire_desired_snapshot(
        runner,
        work_dir=work_dir,
        repository=args.repository,
        requested_tag=args.tag,
        local_manifest=args.manifest,
        local_assets_dir=args.assets_dir,
    )
    desired = snapshot.desired
    asset_hashes = validate_release_assets(desired, snapshot.assets_dir)
    report.update(
        {
            "desired_version": desired.package_version,
            "release_version": desired.version,
            "tag": desired.tag,
            "release_commit": desired.commit,
            "manifest_sha256": desired.manifest_sha256,
            "asset_sha256": asset_hashes,
            "drift_age_seconds": _release_age_seconds(snapshot.release),
        }
    )

    package_dir = args.output_dir if args.render_only else work_dir / "desired-package"
    if package_dir is None:
        permanent("--render-only requires --output-dir")
    materialize_package(
        desired=desired,
        template_dir=args.template_dir,
        output_dir=package_dir,
    )
    generate_srcinfo_and_verify(
        runner,
        package_dir=package_dir,
        image=args.arch_image,
    )
    rendered_version = _srcinfo_package_version(package_dir / ".SRCINFO")
    if rendered_version != desired.package_version:
        permanent(
            f"generated .SRCINFO version {rendered_version!r} does not match "
            f"desired {desired.package_version!r}"
        )

    if args.render_only:
        report.update(
            {
                "classification": Classification.UPDATED.value,
                "action": "render-only",
                "needs_publish": False,
                "published": False,
                "message": f"rendered and verified {package_dir}",
            }
        )
        return

    rpc_state = fetch_rpc_state(runner)
    clone_dir = work_dir / "aur-git"
    observed = inspect_observed_repo(
        runner,
        clone_dir=clone_dir,
        desired_package_dir=package_dir,
        rpc_state=rpc_state,
    )
    report.update(
        {
            "observed_rpc_version": observed.rpc_version,
            "observed_git_version": observed.git_version,
            "remote_commit": observed.git_head,
        }
    )
    compare = VersionComparator(runner, args.arch_image)
    plan = plan_reconciliation(desired.package_version, observed, compare)
    report["action"] = plan.action.value
    report["message"] = plan.reason

    if plan.action is PlanAction.REFUSE_DOWNGRADE:
        raise ReconcileFailure(plan.reason, Classification.REFUSED_DOWNGRADE)
    if plan.action is PlanAction.NOOP:
        report.update(
            {
                "classification": Classification.NOOP.value,
                "needs_publish": False,
                "published": False,
            }
        )
        return
    if plan.action is PlanAction.WAIT_RPC:
        state: RpcState | None = None

        def fetch_version() -> str:
            nonlocal state
            state = fetch_rpc_state(runner)
            return state.version

        _, polls = poll_rpc_version(
            fetch_version,
            desired_version=desired.package_version,
            compare=compare,
            max_attempts=args.rpc_poll_attempts,
            delay=args.rpc_poll_delay,
        )
        report.update(
            {
                "classification": Classification.NOOP.value,
                "observed_rpc_version": state.version if state else observed.rpc_version,
                "rpc_polls": polls,
                "needs_publish": False,
                "published": False,
                "message": "AUR RPC converged to the already-published git state",
            }
        )
        return

    diff = stage_desired_repo(
        runner, clone_dir=clone_dir, desired_package_dir=package_dir
    )
    if not diff.strip():
        permanent(f"{plan.action.value} plan produced no staged AUR diff")
    print("--- mcpp-bin desired-state diff ---")
    print(diff, end="" if diff.endswith("\n") else "\n")
    report["needs_publish"] = True
    if not args.publish:
        report.update(
            {
                "classification": Classification.UPDATED.value,
                "published": False,
                "message": f"{plan.action.value} dry-run validated; publish required",
            }
        )
        return

    remote_commit, retries = publish_staged_repo(
        runner,
        clone_dir=clone_dir,
        desired_version=desired.version,
        max_attempts=args.push_attempts,
        base_delay=args.push_base_delay,
    )
    state: RpcState | None = None

    def fetch_version_after_push() -> str:
        nonlocal state
        state = fetch_rpc_state(runner)
        return state.version

    _, polls = poll_rpc_version(
        fetch_version_after_push,
        desired_version=desired.package_version,
        compare=compare,
        max_attempts=args.rpc_poll_attempts,
        delay=args.rpc_poll_delay,
    )
    clean_arch_install_smoke(
        runner,
        package_dir=package_dir,
        desired_version=desired.version,
        image=args.arch_image,
    )
    report.update(
        {
            "classification": Classification.UPDATED.value,
            "observed_rpc_version": state.version if state else desired.package_version,
            "remote_commit": remote_commit,
            "push_retries": retries,
            "rpc_polls": polls,
            "needs_publish": False,
            "published": True,
            "message": "AUR git, RPC, and clean Arch install converged",
        }
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default=GITHUB_REPOSITORY)
    parser.add_argument("--tag", help="optional exact latest complete stable tag")
    parser.add_argument("--publish", action="store_true", help="push an approved diff")
    parser.add_argument("--trigger", default="manual")
    parser.add_argument("--manifest", type=Path, help="local audit manifest")
    parser.add_argument("--assets-dir", type=Path, help="local audited release assets")
    parser.add_argument("--template-dir", type=Path, default=script_dir / "mcpp-bin")
    parser.add_argument("--render-only", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--arch-image", default=ARCH_IMAGE)
    parser.add_argument("--push-attempts", type=int, default=4)
    parser.add_argument("--push-base-delay", type=float, default=5.0)
    parser.add_argument("--rpc-poll-attempts", type=int, default=12)
    parser.add_argument("--rpc-poll-delay", type=float, default=5.0)
    parser.add_argument("--report-json", type=Path)
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args(argv)
    if args.assets_dir is not None and args.manifest is None:
        parser.error("--assets-dir requires --manifest")
    if args.render_only and args.output_dir is None:
        parser.error("--render-only requires --output-dir")
    if args.publish and args.render_only:
        parser.error("--publish and --render-only are mutually exclusive")
    return args


EXIT_CODES = {
    Classification.NOOP: 0,
    Classification.UPDATED: 0,
    Classification.TRANSIENT: 75,
    Classification.PERMANENT: 2,
    Classification.REFUSED_DOWNGRADE: 3,
}


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    report: dict[str, Any] = {
        "schema": 1,
        "package": AUR_PACKAGE,
        "trigger": args.trigger,
        "classification": Classification.PERMANENT.value,
        "action": None,
        "desired_version": None,
        "observed_rpc_version": None,
        "observed_git_version": None,
        "manifest_sha256": None,
        "asset_sha256": {},
        "remote_commit": None,
        "push_retries": 0,
        "rpc_polls": 0,
        "drift_age_seconds": None,
        "needs_publish": False,
        "published": False,
        "message": None,
    }
    failure: ReconcileFailure | None = None
    runner = CommandRunner()
    try:
        if args.work_dir is not None:
            args.work_dir.mkdir(parents=True, exist_ok=True)
            reconcile_once(args, runner=runner, work_dir=args.work_dir, report=report)
        else:
            with tempfile.TemporaryDirectory(prefix="mcpp-aur-reconcile-") as raw:
                reconcile_once(args, runner=runner, work_dir=Path(raw), report=report)
    except RenderError as exc:
        failure = ReconcileFailure(str(exc), Classification.PERMANENT)
    except ReconcileFailure as exc:
        failure = exc
    except Exception as exc:  # defensive boundary: always emit classification/report
        failure = ReconcileFailure(
            f"unexpected reconciler failure: {type(exc).__name__}: {exc}",
            Classification.PERMANENT,
        )

    if failure is not None:
        report["classification"] = failure.classification.value
        report["message"] = str(failure)
    try:
        write_report(report, args.report_json, args.summary)
    except OSError as exc:
        print(f"cannot write AUR reconciliation report: {exc}", file=sys.stderr)
        return EXIT_CODES[Classification.PERMANENT]
    print(json.dumps(report, sort_keys=True))
    classification = Classification(report["classification"])
    if failure is not None:
        print(f"mcpp-bin reconcile error: {failure}", file=sys.stderr)
    return EXIT_CODES[classification]


if __name__ == "__main__":
    raise SystemExit(main())
