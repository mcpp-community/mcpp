#!/usr/bin/env python3
"""check_payload_names.py -- the engine's sources name no more toolchain payloads than the baseline.

WHY THIS EXISTS

Which package a family's toolchain is for a target on a host is data: the
`[[toolchain.payload]]` entries of modules/toolchain-model/data/builtin.toml
(design 2026-11-12 section 16, T3), read by mcpp.toolchain.payloads in both
directions. A payload named in code is a second statement of that fact, which
the next payload has to edit the engine to extend. This check counts.

WHAT IS COUNTED

The vocabulary is the table's own: every entry name without a placeholder that
is not a family name (`gcc`, `llvm` and `msvc` name the option vocabularies the
engine implements). Each is counted as a string literal in the sources that
check_system_names.py counts, outside comments, with the same reader.

THE RATCHET

`payload-names.baseline` records one number per name. More occurrences than the
baseline fail; fewer fail too, with the line to write, so the count only falls.

    check_payload_names.py            compare with the baseline
    check_payload_names.py --update   rewrite the baseline to the counts found
    check_payload_names.py --list N   print every counted line of name N
"""

import importlib.util
import pathlib
import re
import sys
import tomllib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BASELINE = HERE / "payload-names.baseline"
TABLE = ROOT / "modules/toolchain-model/data/builtin.toml"
FAMILIES = {"gcc", "llvm", "msvc"}

_spec = importlib.util.spec_from_file_location("check_system_names", HERE / "check_system_names.py")
system_names = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(system_names)


def vocabulary():
    entries = tomllib.loads(TABLE.read_text(encoding="utf-8")).get("toolchain", {}).get("payload", [])
    names = []
    for e in entries:
        name = e["name"]
        if "{" not in name and name not in FAMILIES and name not in names:
            names.append(name)
    return names


def count(names):
    patterns = {n: re.compile(r'"%s"' % re.escape(n)) for n in names}
    counts = {n: 0 for n in names}
    lines = {n: [] for n in names}
    for rel, path in system_names.sources():
        state = {"block": False}
        for number, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code = system_names.code_of(raw, state)
            for name, pattern in patterns.items():
                hits = len(pattern.findall(code))
                if hits:
                    counts[name] += hits
                    lines[name].append(f"{rel}:{number}: {raw.strip()}")
    return counts, lines


def main(argv):
    names = vocabulary()
    counts, lines = count(names)
    if len(argv) == 2 and argv[0] == "--list":
        print("\n".join(lines.get(argv[1], [])))
        return 0
    if argv == ["--update"]:
        text = ("# One number per payload name of data/builtin.toml: its occurrences in the\n"
                "# engine's sources (.github/tools/check_payload_names.py). The numbers only fall.\n")
        text += "".join(f"{n} {counts[n]}\n" for n in names)
        BASELINE.write_text(text)
        print(f"wrote {BASELINE.relative_to(ROOT)}")
        return 0
    base = {}
    if BASELINE.is_file():
        for line in BASELINE.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                name, number = line.split()
                base[name] = int(number)
    more = [f"  {n}: {counts[n]} occurrences, the baseline is {base.get(n, 0)}"
            for n in names if counts[n] > base.get(n, 0)]
    fewer = [f"  {n}: {counts[n]} occurrences, the baseline is {base.get(n, 0)}"
             for n in names if counts[n] < base.get(n, 0)]
    if more:
        print("the engine's sources name a toolchain payload more often than the baseline:")
        print("\n".join(more))
        print("Which payload serves a family is data: an entry of `[[toolchain.payload]]`\n"
              "(modules/toolchain-model/data/builtin.toml), read through mcpp.toolchain.payloads.\n"
              "`check_payload_names.py --list <name>` prints the counted lines.")
    if fewer:
        print("the engine's sources name a toolchain payload less often than the baseline; record it:")
        print("\n".join(fewer))
        print("  python3 .github/tools/check_payload_names.py --update")
    return 1 if more or fewer else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
