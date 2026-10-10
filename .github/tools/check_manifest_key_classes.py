#!/usr/bin/env python3
"""The key registry and the documents that list its keys agree (SPEC-004 §2).

`modules/manifest/src/key_registry.cppm` states the class of every key once,
and the parser reads its `[build]` keys from there. A key the registry adds
and the field reference does not mention, or a shared table the workspace
chapter does not list, is the drift this check stops:

  - every `[build]` key of the registry is named in docs/04 and docs/zh/04;
  - every table the registry marks shared has a `[workspace.<table>]` row in
    docs/07 and docs/zh/07 (`profile.<name>` as `[workspace.profile.<name>]`,
    the `target.<sel>` rows as `[workspace.target.<selector>]`).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
ROW = re.compile(r'\{"([^"]+)",\s*"([^"]*)",\s*KeyScope::(\w+),\s*KeyMerge::(\w+),\s*(true|false),\s*(true|false)\s*\}')


def main() -> int:
    rows = ROW.findall((ROOT / "modules/manifest/src/key_registry.cppm").read_text())
    if not rows:
        print("FAIL: no rows read from key_registry.cppm")
        return 1
    problems = []
    for lang in ("", "zh/"):
        ref = (ROOT / f"docs/{lang}04-mcpp-toml.md").read_text()
        ws = (ROOT / f"docs/{lang}07-workspace.md").read_text()
        for table, key, _scope, _merge, shared, _cond in rows:
            if table == "build" and key and f"`{key}`" not in ref and f"{key} " not in ref:
                problems.append(f"docs/{lang}04-mcpp-toml.md does not name [build] {key}")
            if shared == "true" and not key:
                mirror = {
                    "profile.<name>": "[workspace.profile.<name>]",
                    "package": "[workspace.package]",
                }.get(table)
                if table.startswith("target.<sel>"):
                    mirror = "[workspace.target.<selector>]"
                if mirror is None:
                    mirror = f"[workspace.{table}"
                if mirror not in ws:
                    problems.append(f"docs/{lang}07-workspace.md does not list {mirror}")
    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    print(f"OK: {len(rows)} registry rows agree with docs/04 and docs/07 (en, zh)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
