#!/usr/bin/env python3
"""The documentation states the default toolchain the resolver picks on this
host (the 2026-09-28 ecosystem design, WS8).

WHY THIS EXISTS

docs/01 and docs/20 state, in two languages, which toolchain a first run
installs on each host. The review of 2026-09-28 found them stating llvm@20.1.7
while the readings it had in hand said llvm@22.1.8, and nothing compared the
two. The answer now has one authority, `pins::host_default_toolchain`, reported
by `mcpp self env --format json` as `data.defaultToolchain`; this script reads
that report on the host it runs on and checks the four statements of that
host's row against it. Each CI host row runs it, so every row of the tables is
checked on the machine it describes.

Usage:
    python3 .github/tools/check_default_toolchain_docs.py --mcpp <binary>
    python3 .github/tools/check_default_toolchain_docs.py --spec gcc@16.1.0 --os Linux --arch x86_64 [--root DIR]
The second form is for the fixture tests (tests/scripts/).
"""
from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
import sys
from pathlib import Path


def normalise(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def expected_phrases(spec: str, os_name: str, arch: str) -> dict[str, list[str]]:
    """The statements of this host's row, per file, with `spec` in place."""
    s = f"`{spec}`"
    if os_name == "Linux":
        if arch in ("x86_64", "amd64"):
            return {
                "docs/01-getting-started.md": [f"| Linux x86_64 | {s} |"],
                "docs/zh/01-getting-started.md": [f"| Linux x86_64 | {s} |"],
                "docs/20-toolchains.md": [f"- Linux x86_64 uses {s}"],
                "docs/zh/20-toolchains.md": [f"- Linux x86_64 使用面向原生 glibc ABI 的 {s}"],
            }
        if arch in ("aarch64", "arm64"):
            return {
                "docs/01-getting-started.md": [f"| Linux aarch64 | {s} |"],
                "docs/zh/01-getting-started.md": [f"| Linux aarch64 | {s} |"],
                "docs/20-toolchains.md": [f"- Linux aarch64 uses {s}"],
                "docs/zh/20-toolchains.md": [f"- Linux aarch64 使用面向原生 glibc ABI 的 {s}"],
            }
        return {
            "docs/01-getting-started.md": [f"| other Linux architectures | {s} |"],
            "docs/zh/01-getting-started.md": [f"| 其它 Linux 架构 | {s} |"],
            "docs/20-toolchains.md": [f"- Other Linux architectures use {s}"],
            "docs/zh/20-toolchains.md": [f"- 其他 Linux 架构使用 {s}"],
        }
    if os_name == "Darwin":
        return {
            "docs/01-getting-started.md": [f"| macOS | {s} |"],
            "docs/zh/01-getting-started.md": [f"| macOS | {s} |"],
            "docs/20-toolchains.md": [f"- macOS uses {s}."],
            "docs/zh/20-toolchains.md": [f"- macOS 使用 {s}。"],
        }
    if os_name.startswith(("Windows", "MINGW", "MSYS", "CYGWIN")):
        if spec.startswith("gcc@"):
            return {
                "docs/01-getting-started.md": [f"| Windows without it | {s} for `x86_64-windows-gnu` |"],
                "docs/zh/01-getting-started.md": [f"| 没有 MSVC 的 Windows | 面向 `x86_64-windows-gnu` 的 {s} |"],
                "docs/20-toolchains.md": [f"Without usable MSVC, it uses {s} with target `x86_64-windows-gnu`"],
                "docs/zh/20-toolchains.md": [f"没有可用 MSVC 时使用 {s}，target 为 `x86_64-windows-gnu`"],
            }
        return {
            "docs/01-getting-started.md": [f"| Windows with usable MSVC | {s} |"],
            "docs/zh/01-getting-started.md": [f"| 有可用 MSVC 的 Windows | {s} |"],
            "docs/20-toolchains.md": [f"- Windows with a usable MSVC installation uses {s} for the MSVC ABI."],
            "docs/zh/20-toolchains.md": [f"- Windows 上存在可用 MSVC 时使用面向 MSVC ABI 的 {s}"],
        }
    raise SystemExit(f"FAIL: no documented row for host {os_name} {arch}")


def reported_default(mcpp: str) -> str:
    out = subprocess.run([mcpp, "self", "env", "--format", "json"],
                         capture_output=True, text=True, check=False)
    if out.returncode != 0:
        raise SystemExit(f"FAIL: `{mcpp} self env --format json` exited {out.returncode}: {out.stderr.strip()}")
    data = json.loads(out.stdout).get("data", {})
    spec = data.get("defaultToolchain", "")
    if not spec:
        raise SystemExit("FAIL: `self env --format json` reports no defaultToolchain")
    return spec


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcpp")
    ap.add_argument("--spec")
    ap.add_argument("--os", default=platform.system())
    ap.add_argument("--arch", default=platform.machine())
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    if not args.spec and not args.mcpp:
        ap.error("either --mcpp or --spec is required")
    spec = args.spec or reported_default(args.mcpp)
    root = Path(args.root)
    problems = []
    for rel, phrases in expected_phrases(spec, args.os, args.arch).items():
        text = normalise((root / rel).read_text(encoding="utf-8"))
        for phrase in phrases:
            if normalise(phrase) not in text:
                problems.append(f"{rel}: does not state `{phrase}`")
    for p in problems:
        print(f"FAIL: {p}")
    print(f"defaultToolchain on {args.os} {args.arch}: {spec}; {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
