#!/usr/bin/env python3
"""Resolve a sealed release and notify repositories that maintain packages."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from generate_manifest import COMMIT_RE, ManifestError, load_release, required_primary_assets


TARGETS = {
    "homebrew": ("mcpp-community/homebrew-mcpp", "HOMEBREW_TAP_TOKEN"),
    "nix": ("mcpp-community/nix-mcpp", "NIX_PUBLISH_TOKEN"),
}
STABLE_VERSION_RE = re.compile(r"[0-9]+(?:\.[0-9]+)+\Z")


def validate_identity(tag: str, version: str, commit: str) -> None:
    if not STABLE_VERSION_RE.fullmatch(version) or tag != f"v{version}":
        raise ManifestError("expected matching stable numeric version and v-prefixed tag")
    if not COMMIT_RE.fullmatch(commit):
        raise ManifestError("expected a full commit object ID")


def resolve(manifest: dict, expected_tag: str, channel: str) -> dict[str, str]:
    if type(manifest.get("schema")) is not int or manifest["schema"] != 1:
        raise ManifestError("expected release manifest schema 1")
    identity = {key: manifest.get(key) for key in ("tag", "version", "commit")}
    if not all(isinstance(value, str) for value in identity.values()):
        raise ManifestError("manifest tag, version and commit must be strings")
    validate_identity(**identity)
    if expected_tag and identity["tag"] != expected_tag:
        raise ManifestError("manifest does not match the requested tag")
    assets = manifest.get("assets")
    if not isinstance(assets, list):
        raise ManifestError("manifest assets must be an array")
    names = set()
    for asset in assets:
        if not isinstance(asset, dict):
            raise ManifestError("manifest asset must be an object")
        name, digest = asset.get("name"), asset.get("sha256")
        if not isinstance(name, str) or name in names:
            raise ManifestError("manifest asset names must be unique strings")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ManifestError("manifest asset must have a SHA-256 digest")
        names.add(name)
    if not required_primary_assets(identity["version"]).issubset(names):
        raise ManifestError("manifest is missing required platform assets")
    channels = TARGETS if channel == "all" else {channel: TARGETS[channel]}
    identity["matrix"] = json.dumps({"include": [
        {"channel": name, "token": token} for name, (_, token) in channels.items()
    ]}, separators=(",", ":"))
    return identity


def report(message: str) -> None:
    print(message)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(message + "\n")


def notify(channel: str, tag: str, version: str, commit: str) -> int:
    validate_identity(tag, version, commit)
    repository, token_name = TARGETS[channel]
    if not os.environ.get("GH_TOKEN"):
        report(f"Skipped {channel} notification for {tag}: {token_name} is not configured. "
               "The downstream scheduled update remains available.")
        return 0
    payload = {"event_type": "mcpp-release", "client_payload": {
        "version": version, "tag": tag, "commit": commit,
        "manifest_url": f"https://github.com/mcpp-community/mcpp/releases/download/{tag}/mcpp-release.json",
    }}
    result = subprocess.run([
        "gh", "api", "--method", "POST", f"repos/{repository}/dispatches", "--input", "-",
    ], input=json.dumps(payload), text=True, check=False)
    if result.returncode:
        report(f"Failed to notify {channel} ({repository}) for {tag}; retry this channel manually.")
    else:
        report(f"Notification accepted by {channel} ({repository}) for {tag}. "
               f"Publication is verified by the [downstream workflow](https://github.com/{repository}/actions).")
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    resolver = commands.add_parser("resolve")
    resolver.add_argument("--manifest", type=Path, required=True)
    resolver.add_argument("--expected-tag", default="")
    resolver.add_argument("--channel", choices=["all", *TARGETS], default="all")
    notifier = commands.add_parser("notify")
    notifier.add_argument("--channel", choices=list(TARGETS), required=True)
    for key in ("tag", "version", "commit"):
        notifier.add_argument(f"--{key}", required=True)
    args = parser.parse_args()
    try:
        if args.command == "notify":
            return notify(args.channel, args.tag, args.version, args.commit)
        values = resolve(load_release(args.manifest), args.expected_tag, args.channel)
        for key, value in values.items():
            print(f"{key}={value}")
        if output := os.environ.get("GITHUB_OUTPUT"):
            with open(output, "a", encoding="utf-8") as stream:
                for key, value in values.items():
                    stream.write(f"{key}={value}\n")
        return 0
    except (ManifestError, OSError) as exc:
        print(f"package notification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
