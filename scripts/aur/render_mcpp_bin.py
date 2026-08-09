#!/usr/bin/env python3
"""Render the mcpp-bin AUR package from an immutable release manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn


VERSION_RE = re.compile(r"[0-9][0-9A-Za-z.+-]*\Z")
COMMIT_RE = re.compile(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})\Z")
TOKEN_RE = re.compile(r"[A-Za-z0-9_]+\Z")
SHA256_RE = re.compile(r"[0-9a-fA-F]{64}\Z")


class RenderError(RuntimeError):
    """The release manifest or package template violates its contract."""


def fail(message: str) -> NoReturn:
    raise RenderError(message)


@dataclass(frozen=True)
class ReleaseAsset:
    platform: str
    arch: str
    name: str
    sha256: str


@dataclass(frozen=True)
class DesiredState:
    schema: int
    version: str
    tag: str
    commit: str
    assets: tuple[ReleaseAsset, ...]
    manifest_sha256: str

    @property
    def package_version(self) -> str:
        return f"{self.version}-1"

    def asset(self, platform: str, arch: str) -> ReleaseAsset:
        matches = [
            asset
            for asset in self.assets
            if asset.platform == platform and asset.arch == arch
        ]
        if len(matches) != 1:
            fail(
                f"manifest requires exactly one {platform}/{arch} asset, "
                f"found {len(matches)}"
            )
        return matches[0]


def _string_field(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"manifest field {field!r} must be a non-empty string")
    return value


def load_desired_state(path: Path) -> DesiredState:
    try:
        raw_bytes = path.read_bytes()
        manifest = json.loads(raw_bytes)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"cannot read release manifest {path}: {exc}")
    if not isinstance(manifest, dict):
        fail("release manifest root must be an object")
    schema = manifest.get("schema")
    if type(schema) is not int or schema != 1:
        fail(f"unsupported release manifest schema: {schema!r}")

    version = _string_field(manifest.get("version"), "version")
    tag = _string_field(manifest.get("tag"), "tag")
    commit = _string_field(manifest.get("commit"), "commit")
    if VERSION_RE.fullmatch(version) is None:
        fail(f"invalid manifest version: {version!r}")
    if tag != f"v{version}":
        fail(f"manifest tag/version mismatch: {tag!r} versus {version!r}")
    if COMMIT_RE.fullmatch(commit) is None:
        fail("manifest commit must be a full 40- or 64-hex object ID")

    raw_assets = manifest.get("assets")
    if not isinstance(raw_assets, list):
        fail("manifest assets must be an array")
    assets: list[ReleaseAsset] = []
    identities: set[tuple[str, str]] = set()
    names: set[str] = set()
    for index, row in enumerate(raw_assets):
        if not isinstance(row, dict):
            fail(f"manifest asset #{index} must be an object")
        platform = _string_field(row.get("platform"), f"assets[{index}].platform")
        arch = _string_field(row.get("arch"), f"assets[{index}].arch")
        name = _string_field(row.get("name"), f"assets[{index}].name")
        digest = _string_field(row.get("sha256"), f"assets[{index}].sha256")
        if TOKEN_RE.fullmatch(platform) is None or TOKEN_RE.fullmatch(arch) is None:
            fail(f"invalid platform/arch token in manifest asset #{index}")
        if Path(name).name != name:
            fail(f"manifest asset name must be a basename: {name!r}")
        if SHA256_RE.fullmatch(digest) is None:
            fail(f"invalid SHA256 in manifest asset #{index}")
        identity = (platform, arch)
        if identity in identities:
            fail(f"duplicate manifest platform/arch: {platform}/{arch}")
        if name in names:
            fail(f"duplicate manifest asset name: {name}")
        identities.add(identity)
        names.add(name)
        assets.append(ReleaseAsset(platform, arch, name, digest.lower()))

    desired = DesiredState(
        schema=schema,
        version=version,
        tag=tag,
        commit=commit.lower(),
        assets=tuple(assets),
        manifest_sha256=hashlib.sha256(raw_bytes).hexdigest(),
    )
    for arch in ("x86_64", "aarch64"):
        asset = desired.asset("linux", arch)
        expected_name = f"mcpp-{version}-linux-{arch}.tar.gz"
        if asset.name != expected_name:
            fail(
                f"manifest Linux {arch} asset must be {expected_name!r}, "
                f"got {asset.name!r}"
            )
    return desired


def _replace_once(text: str, pattern: str, replacement: str, label: str) -> str:
    rendered, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        fail(f"mcpp-bin PKGBUILD must contain exactly one {label}")
    return rendered


def render_pkgbuild(template: str, desired: DesiredState) -> str:
    x86 = desired.asset("linux", "x86_64")
    arm = desired.asset("linux", "aarch64")
    rendered = _replace_once(
        template, r"^pkgver=.*$", f"pkgver={desired.version}", "pkgver"
    )
    rendered = _replace_once(rendered, r"^pkgrel=.*$", "pkgrel=1", "pkgrel")
    rendered = _replace_once(
        rendered,
        r"^sha256sums_x86_64=.*$",
        f"sha256sums_x86_64=('{x86.sha256}')",
        "sha256sums_x86_64",
    )
    rendered = _replace_once(
        rendered,
        r"^sha256sums_aarch64=.*$",
        f"sha256sums_aarch64=('{arm.sha256}')",
        "sha256sums_aarch64",
    )
    return rendered


def materialize_package(
    *, desired: DesiredState, template_dir: Path, output_dir: Path
) -> None:
    if template_dir.name != "mcpp-bin":
        fail(f"refusing non-mcpp-bin template directory: {template_dir}")
    pkgbuild_path = template_dir / "PKGBUILD"
    launcher_path = template_dir / "mcpp.sh"
    try:
        template = pkgbuild_path.read_text(encoding="utf-8")
        launcher = launcher_path.read_bytes()
        output_dir.mkdir(parents=True, exist_ok=True)
        (output_dir / "PKGBUILD").write_text(
            render_pkgbuild(template, desired), encoding="utf-8"
        )
        (output_dir / "mcpp.sh").write_bytes(launcher)
    except OSError as exc:
        fail(f"cannot materialize mcpp-bin package: {exc}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    default_template = Path(__file__).resolve().parent / "mcpp-bin"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--template-dir", type=Path, default=default_template)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        desired = load_desired_state(args.manifest)
        materialize_package(
            desired=desired,
            template_dir=args.template_dir,
            output_dir=args.output_dir,
        )
    except RenderError as exc:
        print(f"mcpp-bin render error: {exc}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "version": desired.version,
                "package_version": desired.package_version,
                "manifest_sha256": desired.manifest_sha256,
                "output_dir": str(args.output_dir),
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
