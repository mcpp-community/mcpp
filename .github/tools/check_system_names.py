#!/usr/bin/env python3
"""check_system_names.py -- the engine's sources name no more systems than the baseline.

WHY THIS EXISTS

AGENTS.md section 2 states that the engine names no operating system: target
rows, providers and systems are data (modules/toolchain-model/data/builtin.toml,
SPEC-016), and code that is specific to one host lives in modules/platform/.
A review cannot hold that sentence: a branch on `"freebsd"` reads as an
ordinary line, and several entered the tree beside the tables that were meant
to replace them (design 2026-11-12, cause R1). This check counts instead.

WHAT IS COUNTED

Each word of the vocabulary below, in the engine's sources -- `src/` and
`modules/*/src/`, without `modules/platform/` and without tests and data --
in the two forms a branch on a system takes: the word as a string literal
(`"freebsd"`), and the host predicates of mcpplibs.os (`is_linux`,
`is_windows`, `is_macos`). Comments are not counted.

THE RATCHET

`system-names.baseline` records one number per word. More occurrences than the
baseline fail: the new line states a system fact, which belongs in a row, in
the system table or in a provider. Fewer occurrences fail as well, with the
line to write: the baseline follows the code down, so the count never returns.

    check_system_names.py            compare with the baseline
    check_system_names.py --update   rewrite the baseline to the counts found
    check_system_names.py --list W   print every counted line of word W
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE = pathlib.Path(__file__).with_name("system-names.baseline")

LITERALS = ["linux", "macos", "windows", "freebsd", "android", "emscripten", "ios",
            "darwin", "netbsd", "openbsd", "haiku", "dos", "uefi"]
PREDICATES = ["is_linux", "is_windows", "is_macos"]

SOURCE_SUFFIXES = {".cppm", ".cpp", ".mcpp"}


def sources():
    for base in (ROOT / "src", ROOT / "modules"):
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel.startswith("modules/platform/"):
                continue
            parts = rel.split("/")
            if "tests" in parts or "target" in parts or "data" in parts:
                continue
            yield rel, path


def code_of(line, state):
    """The line without its comments. `state["block"]` carries a /* */ across lines."""
    out = []
    i = 0
    in_string = False
    while i < len(line):
        if state["block"]:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out)
            state["block"] = False
            i = end + 2
            continue
        ch = line[i]
        if in_string:
            out.append(ch)
            if ch == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            out.append(ch)
            i += 1
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            state["block"] = True
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def count():
    patterns = {w: re.compile(r'"%s"' % re.escape(w)) for w in LITERALS}
    patterns.update({w: re.compile(r"\b%s\b" % w) for w in PREDICATES})
    counts = {w: 0 for w in patterns}
    lines = {w: [] for w in patterns}
    for rel, path in sources():
        state = {"block": False}
        for n, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code = code_of(raw, state)
            if not code.strip():
                continue
            for word, pat in patterns.items():
                hits = len(pat.findall(code))
                if hits:
                    counts[word] += hits
                    lines[word].append(f"{rel}:{n}: {raw.strip()}")
    return counts, lines


def read_baseline():
    base = {}
    if not BASELINE.is_file():
        return base
    for line in BASELINE.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        word, number = line.split()
        base[word] = int(number)
    return base


def write_baseline(counts):
    text = ("# One number per word: its occurrences in the engine's sources\n"
            "# (.github/tools/check_system_names.py). The numbers only fall.\n")
    for word in LITERALS + PREDICATES:
        text += f"{word} {counts[word]}\n"
    BASELINE.write_text(text)


def main(argv):
    counts, lines = count()
    if len(argv) == 2 and argv[0] == "--list":
        print("\n".join(lines.get(argv[1], [])))
        return 0
    if argv == ["--update"]:
        write_baseline(counts)
        print(f"wrote {BASELINE.relative_to(ROOT)}")
        return 0
    base = read_baseline()
    more, fewer = [], []
    for word in LITERALS + PREDICATES:
        was, now = base.get(word, 0), counts[word]
        if now > was:
            more.append(f"  {word}: {now} occurrences, the baseline is {was}")
        elif now < was:
            fewer.append(f"  {word}: {now} occurrences, the baseline is {was}")
    if more:
        print("the engine's sources name a system more often than the baseline:")
        print("\n".join(more))
        print("A fact of a system is data: a key of its row, of the system table or of a provider\n"
              "(modules/toolchain-model/data/builtin.toml, SPEC-016), read where it was branched on.\n"
              "Code specific to one host lives in modules/platform/.\n"
              "`check_system_names.py --list <word>` prints the counted lines.")
    if fewer:
        print("the engine's sources name a system less often than the baseline; record it:")
        print("\n".join(fewer))
        print("  python3 .github/tools/check_system_names.py --update")
    return 1 if more or fewer else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
