#!/bin/sh
# POSIX sh: under run-as there is only the system shell.
# Runs inside a Termux userland (termux-docker, or an app sandbox through
# run-as). Prints what it sees; asserts nothing.
set -u
OUT="${PROBE_OUT:-/mnt/logs}"
TARBALL="${PROBE_TARBALL:-/mnt/dl/mcpp.tar.gz}"
export HOME="${PROBE_HOME:-${HOME:-/data/data/com.termux/files/home}}"
mkdir -p "$HOME" "$OUT"
export TMPDIR="${TMPDIR:-$HOME/tmp}"; mkdir -p "$TMPDIR"

echo "ENV ANDROID_ROOT=${ANDROID_ROOT-<unset>} ANDROID_DATA=${ANDROID_DATA-<unset>} PREFIX=${PREFIX-<unset>} TERMUX_VERSION=${TERMUX_VERSION-<unset>}"
echo "ENV HOME=$HOME TMPDIR=$TMPDIR SSL_CERT_FILE=${SSL_CERT_FILE-<unset>}"
echo "ENV linker64: $(ls -l /system/bin/linker64 2>&1 | head -1)"
echo "UNAME $(uname -a 2>&1)"
echo "ID $(id 2>&1)"
echo "ID selinux: $(cat /proc/self/attr/current 2>&1 | tr -d '\0')"

cd "$HOME"
rm -f hl_a hl_b
echo a > hl_a
if ln hl_a hl_b 2>"$TMPDIR/ln.err"; then echo "HARDLINK ok"; else echo "HARDLINK refused: $(cat "$TMPDIR/ln.err")"; fi
rm -f hl_a hl_b

mkdir -p "$HOME/dist"
tar -xzf "$TARBALL" -C "$HOME/dist" 2>&1 | tail -3
MCPP=$(find "$HOME/dist" -path '*/bin/mcpp' -type f | head -1)
echo "MCPP $MCPP"
"$MCPP" --version 2>&1
XL=$(dirname "$(dirname "$MCPP")")/registry/bin/xlings
echo "XLINGS $XL"
"$XL" --version 2>&1 | head -3
echo "XLINGS self init (direct):"
XLINGS_HOME=$(dirname "$XL")/.. "$XL" self init 2>&1 | tail -25
echo "XLINGS exit: $?"
"$MCPP" self env 2>&1 | head -40

mkdir -p "$HOME/hello/src"
cd "$HOME/hello"
printf '[package]\nname = "hello"\nversion = "0.1.0"\n\n[targets.hello]\nkind = "bin"\nmain = "src/main.cpp"\n' > mcpp.toml
printf 'import std;\nint main() { std::println("hello from termux"); }\n' > src/main.cpp

t0=$(date +%s)
MCPP_LOG_LEVEL=info timeout 3600 "$MCPP" build --verbose > "$OUT/first-build.log" 2>&1
rc=$?
cat "$OUT/first-build.log"
echo "TIMING first build: exit $rc, $(( $(date +%s) - t0 )) s"
t0=$(date +%s)
timeout 600 "$MCPP" run 2>&1 | tail -5
echo "TIMING run: $(( $(date +%s) - t0 )) s"

mkdir -p "$OUT/mcpp-log"
cp -r "$HOME"/.mcpp/log/. "$OUT/mcpp-log/" 2>/dev/null
cp -r "$(dirname "$(dirname "$MCPP")")"/log/. "$OUT/mcpp-log/" 2>/dev/null
grep -h 'build/stage' "$OUT"/mcpp-log/*.log 2>/dev/null | tail -30 | sed 's/^/STAGE /'
cp "$TMPDIR"/mcpp-xlings-*.stderr "$OUT/" 2>/dev/null
ls "$HOME"/.mcpp/registry/logs/hooks/ 2>/dev/null | head
cp -r "$HOME"/.mcpp/registry/logs "$OUT/registry-logs" 2>/dev/null
cat "$HOME"/.mcpp/config.toml 2>/dev/null | grep -E 'toolchain|target'
exit 0
