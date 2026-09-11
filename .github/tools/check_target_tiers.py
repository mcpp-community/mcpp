#!/usr/bin/env python3
"""Every documented tier agrees with the target table.

WHY THIS EXISTS. `kKnownTargets` in modules/toolchain-model/src/triple.cppm is
the single source for a row's tier, and four documents restate it: both
READMEs, both copies of docs/21. When `wasm32-emscripten` became `verified` and
the Android rows gained tiers, docs/21 was updated and the READMEs were not --
so the front page told a reader that three targets were `planned` while the
engine had built and run two of them. Nothing compared the two, which is the
whole reason it could drift.

THE DENOMINATOR IS THE ENGINE'S TABLE, not the documents'. A check that walked
the documents would pass on a document that lists nothing; this one fails when
a row the engine has is absent from a table that carries tiers at all, and when
a tier disagrees.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "modules/toolchain-model/src/triple.cppm"
TIERS = ("verified", "preview", "planned")

# The engine's answer.
rows = {}
for m in re.finditer(r'^\s*\{\s*"([a-z0-9_.+-]+)",\s*"(verified|preview|planned)"',
                     TABLE.read_text(), re.M):
    rows[m.group(1)] = m.group(2)
if len(rows) < 20:
    sys.exit(f"ERROR: only {len(rows)} rows parsed from {TABLE.name}; "
             "the pattern no longer matches the table")

# Documents that carry a tier column at all. A document without one is not in
# scope -- prose that mentions a target is not a claim about its tier.
docs = [
    ROOT / "README.md",
    ROOT / "README.zh-CN.md",
    ROOT / "docs/21-the-target-triple.md",
    ROOT / "docs/zh/21-the-target-triple.md",
]

fail = False
for doc in docs:
    if not doc.exists():
        print(f"ERROR: {doc.relative_to(ROOT)} is missing")
        fail = True
        continue
    seen = {}
    for line in doc.read_text().splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        tier = next((c for c in cells if c in TIERS), None)
        if tier is None:
            continue
        # Every target named in the row's FIRST cell takes the row's tier.
        for name in re.findall(r"`([a-z0-9_.+-]+)`", cells[0]):
            if name in rows:
                seen[name] = tier
    if not seen:
        print(f"ERROR: {doc.relative_to(ROOT)} is listed here but names no "
              f"target with a tier; either it lost its table or this list is stale")
        fail = True
        continue
    for name, tier in sorted(seen.items()):
        if rows[name] != tier:
            print(f"ERROR: {doc.relative_to(ROOT)}: {name} documented as "
                  f"'{tier}', the table says '{rows[name]}'")
            fail = True
    missing = sorted(set(rows) - set(seen))
    if missing:
        print(f"ERROR: {doc.relative_to(ROOT)} carries tiers but omits "
              f"{len(missing)} row(s): {', '.join(missing)}")
        fail = True
    print(f"  {doc.relative_to(ROOT)}: {len(seen)} of {len(rows)} rows")

if fail:
    sys.exit(1)
print(f"OK: {len(rows)} target tiers agree across {len(docs)} documents")
