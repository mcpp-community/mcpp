#!/usr/bin/env bash
# requires: llvm unix-shell
# 992 -- one file holds the single files of an image for several systems
# (SPEC-014 §5.3; design 2026-10-30 part 2 §19.6).
#
# `--pkg-format polyglot` assembles the single files of the legs and systems
# (`--systems all` unless stated) into one file: the Windows program leads
# with a POSIX shell script in its DOS header, the others follow it. A POSIX
# system starts it through `sh`; the script copies this system's program to the
# single file's cache (SPEC-014 §5.2's rules) once and executes it.
#
# Criteria:
#   A. `mcpp pack --target openkal --pkg-format polyglot -o out/` writes one
#      file, `<name>-<version>.exe`, a PE (MZqFpD, PE at e_lfanew) whose
#      `Runs on` names this machine's pair.
#   B. `sh <file>` runs this machine's program with the arguments; the first
#      run places one program and `.mcpp-complete` under
#      <cache>/mcpp/single-file/<sha256>/; the second places nothing more; a
#      shell that runs a refused file as a script (bash) runs it too.
#   C. A damaged file is refused with status 127 and places nothing.
#   D. A cache directory that is a link is refused with status 127.
#   E. `--systems <os>` without Windows writes a plain script, which the
#      system executes directly.
#   F. `mcpp run --pkg-format polyglot` runs it (through `sh`).
#   G. Refused before a build: `polyglot` in a `--pkg-format` list; a native
#      target; `mcpp run --pkg-format tar`; `mcpp emit xpkg` names the file.
#   H. (wine) the same file runs as a Windows program.
set -e

TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
case "$(uname -s)" in Linux) OS=linux ;; Darwin) OS=macos ;; *) echo "SKIP: a POSIX host"; exit 0 ;; esac

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "pg"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main(int argc, char** argv) { std::println("pg {} args, first [{}]", argc, argc > 1 ? argv[1] : ""); }\n' > src/main.cpp

# A
"$MCPP" pack --target openkal --pkg-format polyglot -o out/ > a.log 2>&1 || fail "A: pack" a.log
[ "$(ls out)" = "pg-0.1.0.exe" ] || fail "A: the files are not one pg-0.1.0.exe: $(ls out)" a.log
F="$TMP/app/out/pg-0.1.0.exe"
[ "$(head -c 8 "$F")" = "MZqFpD='" ] || fail "A: the file does not begin with the DOS header's script" a.log
off=$(od -An -tu4 -j60 -N4 "$F" | tr -d ' ')
[ "$(tail -c +$((off + 1)) "$F" | head -c 2)" = "PE" ] || fail "A: no PE header at e_lfanew ($off)" a.log
grep -q "Runs on .*$OS-$HOST_ARCH" a.log || fail "A: Runs on does not name $OS-$HOST_ARCH" a.log
echo "ok: one file, a PE with the script in its DOS header"

# B -- the cache of an empty home
REAL_HOME="$HOME"
export HOME="$TMP/home"
unset XDG_CACHE_HOME
mkdir -p "$HOME"
if [ "$OS" = macos ]; then CACHE="$HOME/Library/Caches/mcpp/single-file"; else CACHE="$HOME/.cache/mcpp/single-file"; fi
sh "$F" first > b1.log 2>&1 || fail "B: the first run" b1.log
grep -q "pg 2 args, first \[first\]" b1.log || fail "B: the first run's output" b1.log
n1=$(find "$CACHE" -type f | wc -l | tr -d ' ')
[ "$n1" = 2 ] || { find "$CACHE" >&2; fail "B: the first run placed $n1 files, expected the program and .mcpp-complete"; }
ls "$CACHE"/*/.mcpp-complete > /dev/null 2>&1 || fail "B: no .mcpp-complete"
sh "$F" second > b2.log 2>&1 || fail "B: the second run" b2.log
grep -q "first \[second\]" b2.log || fail "B: the second run's output" b2.log
n2=$(find "$CACHE" | wc -l | tr -d ' ')
bash -c "'$F' viabash" > b3.log 2>&1 || fail "B: bash running the file" b3.log
grep -q "first \[viabash\]" b3.log || fail "B: bash's run" b3.log
[ "$(find "$CACHE" | wc -l | tr -d ' ')" = "$n2" ] || fail "B: a later run placed more files"
echo "ok: placed once, run from the cache after"

# C -- one byte of this machine's program changed
# This machine's program lies at `a=` of its line in the script at the end.
D="$TMP/damaged.exe"
cp "$F" "$D"
at=$(tail -c 4096 "$F" | grep -a "k=$OS-$HOST_ARCH " | sed 's/.* a=\([0-9]*\) .*/\1/')
[ -n "$at" ] || fail "C: no line for $OS-$HOST_ARCH in the script"
pos=$((at + 4096))
byte=$(tail -c +$((pos + 1)) "$F" | head -c 1 | od -An -tu1 | tr -d ' ')
printf "\\$(printf '%03o' $(( (byte + 1) % 256 )))" | dd of="$D" bs=1 seek="$pos" conv=notrunc 2>/dev/null
cmp -s "$F" "$D" && fail "C: the byte was not changed"
HOME="$TMP/home-c" XDG_CACHE_HOME= sh "$D" x > c.log 2>&1 && rc=0 || rc=$?
[ "$rc" = 127 ] || fail "C: a damaged file exited $rc, expected 127" c.log
grep -q "does not match its digest" c.log || fail "C: the refusal does not say why" c.log
[ -z "$(find "$TMP/home-c" -name .mcpp-complete 2>/dev/null)" ] || fail "C: a damaged program was placed" c.log
echo "ok: a damaged program is refused"

# D -- a cache directory that is a link
mkdir -p "$TMP/home-d/elsewhere"
if [ "$OS" = macos ]; then mkdir -p "$TMP/home-d/Library/Caches"; ln -s "$TMP/home-d/elsewhere" "$TMP/home-d/Library/Caches/mcpp";
else mkdir -p "$TMP/home-d/.cache"; ln -s "$TMP/home-d/elsewhere" "$TMP/home-d/.cache/mcpp"; fi
HOME="$TMP/home-d" sh "$F" x > d.log 2>&1 && rc=0 || rc=$?
[ "$rc" = 127 ] || fail "D: a linked cache directory exited $rc, expected 127" d.log
grep -q "is not a directory of this user's" d.log || fail "D: the refusal does not say why" d.log
echo "ok: a linked cache directory is refused"

# E -- without Windows the file is a script the system executes
"$MCPP" pack --target openkal --pkg-format polyglot --systems "$OS" -o plain/ > e.log 2>&1 || fail "E: pack" e.log
[ -f plain/pg-0.1.0 ] || fail "E: no pg-0.1.0 (without .exe)" e.log
[ "$(head -c 9 plain/pg-0.1.0)" = "#!/bin/sh" ] || fail "E: not a script" e.log
plain/pg-0.1.0 direct > e-run.log 2>&1 || fail "E: executed directly" e-run.log
grep -q "first \[direct\]" e-run.log || fail "E: the output" e-run.log
echo "ok: a file without Windows is a script"

# F
"$MCPP" run --target "$HOST_ARCH-openkal" --pkg-format polyglot -- viarun > f.log 2>&1 || fail "F: mcpp run" f.log
grep -q "first \[viarun\]" f.log || fail "F: the program's output" f.log
grep -q 'Running `sh ' f.log || fail "F: not started through sh" f.log
echo "ok: mcpp run --pkg-format polyglot"

# G
"$MCPP" pack --target openkal --pkg-format tar,polyglot -o g/ > g1.log 2>&1 && fail "G: a list with polyglot was accepted" g1.log
grep -q "polyglot is not one" g1.log || fail "G: the list's refusal" g1.log
"$MCPP" pack --pkg-format polyglot > g2.log 2>&1 && fail "G: a native target was accepted" g2.log
grep -q "is a program of one system" g2.log || fail "G: the native refusal" g2.log
grep -q "Compiling" g2.log && fail "G: the native refusal came after a build" g2.log
"$MCPP" run --pkg-format tar > g3.log 2>&1 && fail "G: mcpp run --pkg-format tar was accepted" g3.log
grep -q "runs a program, and tar is a container" g3.log || fail "G: run's refusal" g3.log
"$MCPP" emit xpkg --package "$F" --url https://example.invalid > g4.log 2>&1 && fail "G: emit xpkg accepted the file" g4.log
grep -q "is a polyglot file" g4.log || fail "G: emit xpkg does not name the polyglot file" g4.log
echo "ok: the refusals"

# H
if command -v wine > /dev/null 2>&1 && [ "$OS" = linux ]; then
    HOME="$REAL_HOME" WINEDEBUG=-all timeout 300 wine "$F" onwine > h.log 2>&1 || fail "H: wine" h.log
    grep -q "first \[onwine\]" h.log || fail "H: the Windows program's output" h.log
    echo "ok: the file is a Windows program"
fi

echo "OK"
