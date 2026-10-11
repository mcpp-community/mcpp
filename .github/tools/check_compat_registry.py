#!/usr/bin/env python3
"""The deprecation registry, the code and docs/51 agree (mcpp#797).

modules/manifest/src/compat/registry.cppm is the one list of readings a user
can write and that are to be removed. Three things are derived from it and can
drift from it, so each is checked here:

  1. Every `COMPAT(<id>)` marker in the code names a row, and every row has at
     least one marker -- the reading is somewhere, and it is registered.
     `COMPAT(internal:<topic>)` names engine-internal compatibility that no
     manifest can spell; it has no row and is not checked.
  2. docs/51 (English and Chinese) lists every row with its `since` and its
     `floor`.
  3. Each row's fields are present.

The removal itself -- that no reading outlives its floor -- is the unit test
modules/manifest/tests/test_compat_registry.cpp.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "modules/manifest/src/compat/registry.cppm"
DOCS = [ROOT / "docs/51-supported-versions.md", ROOT / "docs/zh/51-supported-versions.md"]


def rows():
    text = REGISTRY.read_text(encoding="utf-8")
    floor = re.search(r'kRemovalFloor\s*=\s*"([^"]+)"', text).group(1)
    body = text[text.index("kDeprecations[] = {"):]
    body = body[:body.index("\n};")]
    out = []
    for block in re.findall(r"\{(.*?)\n    \}", body, re.S):
        fields = re.findall(r'"((?:[^"\\]|\\.)*)"|(kRemovalFloor)', block)
        values = []
        pending = ""
        # Adjacent string literals concatenate, as in C++.
        tokens = re.findall(r'"((?:[^"\\]|\\.)*)"\s*|(kRemovalFloor)|(,)', block)
        for lit, name, comma in tokens:
            if comma:
                values.append(pending)
                pending = ""
            elif name:
                pending = floor
            else:
                pending += lit
        if pending:
            values.append(pending)
        values = [v for v in values]
        out.append(values)
    return out


def main() -> int:
    failures = []
    registered = {}
    for r in rows():
        if len(r) < 7 or not all(r[:7]):
            failures.append(f"registry: a row has an empty field: {r}")
            continue
        registered[r[0]] = (r[1], r[2])

    markers = {}
    for path in list(ROOT.glob("src/**/*.cpp*")) + list(ROOT.glob("modules/**/*.cpp*")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"COMPAT\(([a-z0-9:<>-]+)\)", text):
            ident = m.group(1)
            if ident.startswith("internal:") or ident.startswith("<"):
                continue
            markers.setdefault(ident, []).append(str(path.relative_to(ROOT)))
    for ident, where in sorted(markers.items()):
        if ident not in registered:
            failures.append(f"COMPAT({ident}) in {where[0]} names no row of {REGISTRY.relative_to(ROOT)}")
    for ident in sorted(registered):
        if ident not in markers:
            failures.append(f"the row '{ident}' has no COMPAT({ident}) marker in the code")

    for doc in DOCS:
        text = doc.read_text(encoding="utf-8")
        for ident, (since, floor) in sorted(registered.items()):
            line = next((l for l in text.splitlines() if f"`{ident}`" in l), None)
            if line is None:
                failures.append(f"{doc.relative_to(ROOT)} does not list `{ident}`")
            elif since not in line or floor not in line:
                failures.append(f"{doc.relative_to(ROOT)}: the row of `{ident}` does not state "
                                f"{since} and {floor}")

    for f in failures:
        print(f"FAIL: {f}")
    if failures:
        return 1
    print(f"ok: {len(registered)} deprecations, their markers and docs/51 agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
