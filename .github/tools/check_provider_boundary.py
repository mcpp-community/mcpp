#!/usr/bin/env python3
"""The provider boundary (.agents/docs/2026-10-16-xlings-provider-port-and-shared-store-design.md §4.3).

mcpp speaks the port's words (mcpp.provider): what xlings' commands, its
protocol, its home layout and its store naming are is the adapter's
(src/provider/xlings/), and only the composition root (src/provider/select.cppm)
names the adapter. Outside them, in src/:

  - no `import mcpp.provider.xlings*`;
  - none of the layout's spellings in code: "xpkgs", "-x-", "xim-x-",
    ".xlings.json", "subos", "xim-index-repos", ".mcpp_ok";
  - a payload path is used as the provider answered it: no canonical /
    weakly_canonical / remove_all applied to one.

Comments are prose and may name anything; string literals and identifiers
in code may not. An exception names its reason on the line:
`// provider-boundary-ok: <why>`.

Exit 0 clean, 1 with the findings listed.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

LITERALS = ['"xpkgs"', '/xpkgs', 'xpkgs/', '-x-', 'xim-x-', '.xlings.json', '"subos"', '/subos', 'subos/',
            'xim-index-repos', '.mcpp_ok', '.mcpp-fixup.json']
IMPORT = re.compile(r'^\s*(export\s+)?import\s+mcpp\.provider\.xlings\b')
PAYLOAD_FS = re.compile(r'\b(canonical|weakly_canonical|remove_all)\s*\([^)]*[pP]ayload')


def strip_comments(text: str) -> list[str]:
    """Each line with // and /* */ comments blanked, string literals kept."""
    out, in_block = [], False
    for line in text.split("\n"):
        res, i, in_str = [], 0, None
        while i < len(line):
            c = line[i]
            if in_block:
                if line.startswith("*/", i):
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if in_str:
                res.append(c)
                if c == "\\" and i + 1 < len(line):
                    res.append(line[i + 1])
                    i += 2
                    continue
                if c == in_str:
                    in_str = None
                i += 1
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            if c in "\"'":
                in_str = c
            res.append(c)
            i += 1
        out.append("".join(res))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    ROOT = ap.parse_args().root.resolve()
    SRC = ROOT / "src"
    ADAPTER = SRC / "provider" / "xlings"
    ROOT_MODULE = SRC / "provider" / "select.cppm"
    findings = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".cppm", ".cpp", ".hpp", ".h") or not path.is_file():
            continue
        if ADAPTER in path.parents:
            continue
        raw = path.read_text(encoding="utf-8", errors="replace").split("\n")
        code = strip_comments("\n".join(raw))
        for n, (line, original) in enumerate(zip(code, raw), 1):
            if "provider-boundary-ok:" in original:
                continue
            rel = path.relative_to(ROOT)
            if IMPORT.match(line) and path != ROOT_MODULE:
                findings.append(f"{rel}:{n}: imports the adapter; import mcpp.provider.select")
            for lit in LITERALS:
                if lit in line:
                    findings.append(f"{rel}:{n}: names the provider's layout ({lit}): {original.strip()}")
                    break
            if PAYLOAD_FS.search(line):
                findings.append(f"{rel}:{n}: canonicalises or removes a payload: {original.strip()}")
    if findings:
        print("provider boundary: " + str(len(findings)) + " finding(s)")
        for f in findings:
            print("  " + f)
        return 1
    print("provider boundary: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
