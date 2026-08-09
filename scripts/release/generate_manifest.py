#!/usr/bin/env python3
"""Generate and validate the immutable mcpp GitHub release manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, NoReturn


SCHEMA_VERSION = 1
COMMIT_RE = re.compile(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})\Z")
VERSION_RE = re.compile(r"[0-9][0-9A-Za-z.+-]*\Z")
PRIMARY_ASSET_RE = re.compile(
    r"^mcpp-(?P<version>[0-9][0-9A-Za-z.+-]*)-"
    r"(?P<platform>[A-Za-z][A-Za-z0-9_]*)-"
    r"(?P<arch>[A-Za-z0-9_]+)\."
    r"(?:tar\.gz|zip)$"
)
SIDECAR_RE = re.compile(r"^(?P<sha256>[0-9a-fA-F]{64})[ \t]+\*?(?P<name>\S+)$")


class ManifestError(RuntimeError):
    """A release violates the public manifest contract."""


def fail(message: str) -> NoReturn:
    raise ManifestError(message)


def load_release(path: Path) -> dict[str, Any]:
    try:
        release = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"cannot read release JSON {path}: {exc}")
    if not isinstance(release, dict):
        fail("release JSON root must be an object")
    return release


def release_asset_names(release: dict[str, Any]) -> set[str]:
    raw_assets = release.get("assets")
    if not isinstance(raw_assets, list):
        fail("release JSON assets must be an array")

    names: set[str] = set()
    for index, raw_asset in enumerate(raw_assets):
        if not isinstance(raw_asset, dict):
            fail(f"release asset #{index} must be an object")
        name = raw_asset.get("name")
        if not isinstance(name, str) or not name:
            fail(f"release asset #{index} has no valid name")
        if name in names:
            fail(f"duplicate release asset name: {name}")
        names.add(name)
    return names


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        fail(f"cannot read release asset {path}: {exc}")
    return digest.hexdigest()


def read_sidecar(path: Path, primary_name: str) -> str:
    try:
        lines = [
            line.strip()
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    except (OSError, UnicodeError) as exc:
        fail(f"cannot read sidecar {path}: {exc}")
    if len(lines) != 1:
        fail(f"sidecar must contain exactly one checksum record: {path.name}")
    match = SIDECAR_RE.fullmatch(lines[0])
    if match is None:
        fail(f"invalid SHA256 sidecar syntax: {path.name}")
    if match.group("name") != primary_name:
        fail(
            f"sidecar filename target mismatch: {path.name} names "
            f"{match.group('name')!r}, expected {primary_name!r}"
        )
    return match.group("sha256").lower()


def required_primary_assets(version: str) -> set[str]:
    return {
        f"mcpp-{version}-linux-x86_64.tar.gz",
        f"mcpp-{version}-linux-aarch64.tar.gz",
        f"mcpp-{version}-macosx-arm64.tar.gz",
        f"mcpp-{version}-windows-x86_64.zip",
    }


def generate_manifest(
    *,
    release: dict[str, Any],
    assets_dir: Path,
    version: str,
    tag: str,
    commit: str,
) -> dict[str, Any]:
    if VERSION_RE.fullmatch(version) is None:
        fail(f"invalid version: {version!r}")
    if tag != f"v{version}":
        fail(f"tag/version mismatch: expected v{version!s}, got {tag!r}")
    if COMMIT_RE.fullmatch(commit) is None:
        fail("commit must be a full 40- or 64-hex object ID")
    if release.get("tag_name") != tag:
        fail(
            f"release tag mismatch: release has {release.get('tag_name')!r}, "
            f"expected {tag!r}"
        )
    if release.get("draft") is not False:
        fail("release must have draft=false")
    if release.get("prerelease") is not False:
        fail("release must have prerelease=false")

    names = release_asset_names(release)
    discovered: list[tuple[str, re.Match[str]]] = []
    for name in sorted(names):
        match = PRIMARY_ASSET_RE.fullmatch(name)
        if match is None:
            continue
        if match.group("version") != version:
            fail(
                f"asset version mismatch: {name!r} has "
                f"{match.group('version')!r}, expected {version!r}"
            )
        discovered.append((name, match))

    required = required_primary_assets(version)
    missing = sorted(required - {name for name, _ in discovered})
    if missing:
        fail(f"missing required primary asset(s): {', '.join(missing)}")

    manifest_assets: list[dict[str, str]] = []
    identities: dict[tuple[str, str], str] = {}
    for name, match in discovered:
        platform = "macos" if match.group("platform") == "macosx" else match.group("platform")
        arch = match.group("arch")
        identity = (platform, arch)
        if identity in identities:
            fail(
                f"duplicate platform/arch {platform}/{arch}: "
                f"{identities[identity]} and {name}"
            )
        identities[identity] = name

        sidecar_name = f"{name}.sha256"
        if sidecar_name not in names:
            fail(f"missing sidecar release asset: {sidecar_name}")
        primary_path = assets_dir / name
        sidecar_path = assets_dir / sidecar_name
        if not primary_path.is_file():
            fail(f"missing downloaded primary asset: {primary_path}")
        if not sidecar_path.is_file():
            fail(f"missing downloaded sidecar: {sidecar_path}")

        expected_digest = read_sidecar(sidecar_path, name)
        actual_digest = sha256_file(primary_path)
        if actual_digest != expected_digest:
            fail(
                f"SHA256 mismatch for {name}: sidecar has {expected_digest}, "
                f"payload has {actual_digest}"
            )
        manifest_assets.append(
            {
                "platform": platform,
                "arch": arch,
                "name": name,
                "sha256": actual_digest,
            }
        )

    manifest_assets.sort(key=lambda item: (item["platform"], item["arch"], item["name"]))
    return {
        "schema": SCHEMA_VERSION,
        "version": version,
        "tag": tag,
        "commit": commit.lower(),
        "assets": manifest_assets,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-json", required=True, type=Path)
    parser.add_argument("--assets-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        release = load_release(args.release_json)
        manifest = generate_manifest(
            release=release,
            assets_dir=args.assets_dir,
            version=args.version,
            tag=args.tag,
            commit=args.commit,
        )
        rendered = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    except (ManifestError, OSError) as exc:
        print(f"release manifest error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
