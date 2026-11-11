#!/usr/bin/env bash
# requires: llvm unix-shell
# 1078 -- `--systems all` carries the runners the platform's own data names
# (2026.11.11.1, G5): this machine's system, the rows a platform defines, and
# the systems its runner states a host of -- FreeBSD's among them, whose
# runner is built as <arch>-freebsd-musl over the platform's hosted
# arrangement.
#
# The default is unchanged: a pack without `--systems` carries the runners
# of the legs' own systems -- this machine's system for every architecture
# the package has a leg of -- and no other system's.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -30 "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
case "$(uname -s)" in Linux) OS=linux ;; *) echo "SKIP: a Linux host"; exit 0 ;; esac

cd "$TMP"
"$MCPP" new app > /dev/null 2>&1
cd app

# The default: this machine's system for each leg's architecture, nothing
# else (`mcpp new app` carries an x86_64 and an aarch64 leg). A `dir` pack's
# tree lands under target/dist whatever `-o` says (design 2026-10-29 §4.6),
# so the pack is located, not assumed.
"$MCPP" pack --target openkal --mode self-contained --pkg-format dir > d.log 2>&1 \
    || fail "the default pack did not build" d.log
one=$(find target/dist -mindepth 1 -maxdepth 1 -type d -newer mcpp.toml | head -1)
[ -n "$one" ] || fail "no pack tree under target/dist" d.log
[ -d "$one/libexec/okrun/${OS}-${ARCH}" ] || fail "the default pack carries no runner of its own system" d.log
others=$(ls "$one/libexec/okrun" 2>/dev/null | grep -v "^${OS}-" || true)
[ -z "$others" ] || fail "the default pack carries a system other than this machine's: $others" d.log

# `--systems all`: the data's answer, FreeBSD included. The same tree name
# replaces the default's, so dist is cleared first and the tree that lands
# is the all-pack's own.
rm -rf target/dist
"$MCPP" pack --target openkal --mode self-contained --systems all --pkg-format dir > a.log 2>&1 \
    || fail "--systems all did not pack" a.log
all_=$(find target/dist -mindepth 1 -maxdepth 1 -type d | head -1)
[ -n "$all_" ] || fail "no --systems all tree under target/dist" a.log
for pair in "linux-x86_64" "linux-aarch64" "freebsd-x86_64" "freebsd-aarch64"; do
    [ -d "$all_/libexec/okrun/$pair" ] || fail "no runner carried for $pair" a.log
done
# The Windows row builds wherever this mcpp builds it; where it does not, the
# warning says so instead.
if [ ! -d "$all_/libexec/okrun/windows-x86_64" ]; then
    grep -q "no runner for windows-x86_64" a.log \
        || fail "the Windows runner is neither carried nor said" a.log
fi
echo "PASS: 1078 --systems all carries the runners the platform declares"
