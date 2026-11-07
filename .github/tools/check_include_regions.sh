#!/usr/bin/env bash
# check_include_regions.sh — headers are included only where a system has no
# module for what is needed (design 2026-11-07 D10).
#
# mcpp imports the standard library (`import std`), its dependencies as modules
# (nlohmann.json, mcpplibs.os, the mwrap wrappers of zlib, zstd, xz and of the
# C library's macros), and nothing else. The only `#include` directives of
# `src/` and `modules/` are the operating system's headers of the file lock,
# each in the source file of its system (src/native/<system>/lock.cpp). A
# directive anywhere else is a header a module should provide: depend on the
# package that wraps it, or wrap it (mcpp-index docs/mwrap.md).
#
# Text that only looks like a directive is not one: a raw string (the source
# mcpp writes for a launcher or a template) and a comment are skipped. Tests
# (`tests/`, a module's `tests/`) are outside the rule.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
import re, sys
from pathlib import Path

ALLOWED = {Path("src/native/posix/lock.cpp"), Path("src/native/windows/lock.cpp")}
RAW = re.compile(r'(?:u8|u|U|L)?R"([^()\\ ]{0,16})\(')

def directives(text):
    """Line numbers of `#include` directives outside comments and raw strings."""
    out, i, n, line = [], 0, len(text), 1
    at_line_start = True
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1; at_line_start = True; i += 1; continue
        if text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            line += text.count('\n', i, j); i = j; continue
        m = RAW.match(text, i)
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            end = text.find(')' + m.group(1) + '"', m.end()); end = n if end < 0 else end + len(m.group(1)) + 2
            line += text.count('\n', i, end); i = end; at_line_start = False; continue
        if c == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == '\\' else 1
            i = j + 1; at_line_start = False; continue
        if at_line_start and c == '#':
            if re.match(r'#\s*include\b', text[i:]): out.append(line)
        if not c.isspace(): at_line_start = False
        i += 1
    return out

bad = 0
for path in sorted(list(Path("src").rglob("*.cpp*")) + list(Path("modules").rglob("*.cpp*"))):
    if "tests" in path.parts or "test" in path.parts or path in ALLOWED:
        continue
    for line in directives(path.read_text(encoding="utf-8", errors="replace")):
        print(f"{path}:{line}: #include outside the system lock files; import a module instead")
        bad += 1
for path in ALLOWED:
    if not path.exists():
        print(f"{path}: the allowed region is gone; update this check")
        bad += 1
print(f"check_include_regions: {bad} problem(s)")
sys.exit(1 if bad else 0)
PY
