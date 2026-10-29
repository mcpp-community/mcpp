#!/usr/bin/env bash
# requires: llvm python3 unix-shell
# 986 -- single files cover the legs and the systems (design 2026-10-29 rev 2
# §4.1, §4.5).
#
# `--pkg-format single-file` of an image makes one program per leg and system
# of the leg's architecture: the platform's carrier with the image in it.
# `--systems` names `<os>`, `<os>-<arch>` or `all`; `all` makes a file for
# every row this mcpp builds a program for and says the rest. More than one
# file takes a directory for `-o`. On 2026.10.29.1 `--target openkal` was an
# unknown target and `--systems` took only `<os>`.
#
# Criteria:
#   A. `--target openkal --pkg-format single-file -o files/` writes a file for
#      each architecture on this system, each a program of its architecture;
#      this machine's runs.
#   B. `--systems <os>-<host>` with both legs makes one file.
#   C. (Linux) `--systems all` makes the files of every row this mcpp builds a
#      program for and says each other pair, `Not covered`, with its reason.
#   D. `-o` naming one file with two files is refused before a build.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
case "$(uname -s)" in Linux) OS=linux ;; Darwin) OS=macos ;; *) echo "SKIP: a POSIX host"; exit 0 ;; esac

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname    = "sf"\nversion = "0.1.0"\n' > mcpp.toml
printf 'import std;\nint main(int argc, char**) { std::println("single {} args", argc); }\n' > src/main.cpp

# A
"$MCPP" pack --target openkal --pkg-format single-file -o files/ > a.log 2>&1 || fail "A: pack" a.log
for arch in x86_64 aarch64; do
    [ -f "files/sf-0.1.0-$OS-$arch" ] || fail "A: no file for $OS-$arch" a.log
done
"files/sf-0.1.0-$OS-$HOST_ARCH" x > a-run.log 2>&1 || fail "A: this machine's file" a-run.log
grep -q "single 2 args" a-run.log || fail "A: the file's output" a-run.log
echo "ok: a single file per leg"

# B
"$MCPP" pack --target openkal --pkg-format single-file --systems "$OS-$HOST_ARCH" -o one/ > b.log 2>&1 \
    || fail "B: --systems $OS-$HOST_ARCH" b.log
[ "$(ls one)" = "sf-0.1.0-$OS-$HOST_ARCH" ] || fail "B: the files are not the one pair" b.log
echo "ok: --systems <os>-<arch> makes that pair's file"

# C
if [ "$OS" = linux ]; then
    "$MCPP" pack --target openkal --pkg-format single-file --systems all -o all/ > c.log 2>&1 || fail "C: --systems all" c.log
    for pair in linux-x86_64 linux-aarch64 windows-x86_64 macos-aarch64; do
        ls all/sf-0.1.0-$pair* > /dev/null 2>&1 || fail "C: no file for $pair" c.log
    done
    grep -q "Not covered macos-x86_64 (no carrier row" c.log || fail "C: the pair without a row is not said" c.log
    echo "ok: --systems all makes every file this mcpp can and says the rest"
fi

# D
if "$MCPP" pack --target openkal --pkg-format single-file -o one-file > d.log 2>&1; then
    fail "D: -o one file with two files was accepted" d.log
fi
grep -q "use: -o <directory>/" d.log || fail "D: the refusal" d.log
grep -q "Compiling" d.log && fail "D: refused after compiling" d.log
echo "ok: -o naming one file is refused for two files"

echo "PASS: 986 single files cover the legs and systems"
