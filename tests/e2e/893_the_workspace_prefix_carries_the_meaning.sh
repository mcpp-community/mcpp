#!/usr/bin/env bash
# requires: gcc
# 893 -- the prefix carries the meaning (SPEC-004 §9.10, design 2026-10-10 §3;
# #785, #786).
#
#   P1  `[workspace.target.<sel>.build]` reaches every member: `dialect_cxxflags`
#       (a configuration key, read from the build's root) and `cxxflags` /
#       `defines` (package keys). In 2026.10.8.1 a `[target.<sel>.build]` on
#       the root reached none, without a word (#786).
#   P2  The package keys reach a member at every position: built as a
#       sibling's `path` dependency too (W5).
#   P3  `[workspace.profile.<name>]` merges key by key with a member's own
#       table of that name; the workspace's list comes first (W3).
#   P4  A member's own definition of a macro the workspace defines wins.
#   P5  `[workspace]` refuses a table it does not know, and one table written
#       both at the root position and as `[workspace.X]` on a root without
#       `[package]`.
#   P6  W7: `[build]` on a root without `[package]` acts on nothing and is
#       reported; `--strict` refuses.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

WS="$TMP/ws"
mkdir -p "$WS/lib/src" "$WS/app/src"
cd "$WS"
cat > mcpp.toml <<'TOML'
[workspace]
members = ["lib", "app"]

[workspace.package]
mcpp = ">=2026.10.10.1"

[workspace.target.'cfg(any(os = "linux", os = "macos", os = "windows"))'.build]
dialect_cxxflags = ["-DWS_DIALECT=1"]
cxxflags = ["-DWS_PACKAGE=1"]
defines = ["WS_MACRO=1"]

[workspace.profile.release]
cxxflags = ["-DWS_RELEASE=1"]
TOML
cat > lib/mcpp.toml <<'TOML'
[package]
name = "lib"
version = "0.1.0"

[targets.lib]
kind = "lib"
TOML
cat > lib/src/lib.cppm <<'CPP'
module;
#if !defined(WS_DIALECT) || !defined(WS_PACKAGE)
#error "the workspace layer did not reach lib"
#endif
export module lib;
export int lib_value() { return WS_MACRO; }
CPP
cat > app/mcpp.toml <<'TOML'
[package]
name = "app"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }

[target.'cfg(any(os = "linux", os = "macos", os = "windows"))'.build]
defines = ["WS_MACRO=2"]

[profile.release]
cxxflags = ["-DAPP_RELEASE=1"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
TOML
cat > app/src/main.cpp <<'CPP'
#if !defined(WS_DIALECT) || !defined(WS_PACKAGE)
#error "the workspace layer did not reach app"
#endif
#include <cstdio>
import lib;
int main() { std::printf("macro=%d lib=%d\n", WS_MACRO, lib_value()); }
CPP

# ── P1, P2, P4 ────────────────────────────────────────────────────────────
"$MCPP" build -p app > build.log 2>&1 || fail "the workspace layer build failed" build.log
out=$(find target -path '*/bin/*' -name 'app' -type f -o -path '*/bin/*' -name 'app.exe' -type f | head -1)
[ -n "$out" ] || fail "no app binary" build.log
got=$("$out" | tr -d '\r')
[ "$got" = "macro=2 lib=1" ] \
    || fail "expected app's own WS_MACRO=2 and lib's inherited WS_MACRO=1, got '$got'" build.log
if grep -q "workspace-position\|acts on no package" build.log; then
    fail "the canonical spelling was reported as a deprecated or inert one" build.log
fi
echo "ok P1 P2 P4"

# ── P3 ────────────────────────────────────────────────────────────────────
"$MCPP" build -p app --release > release.log 2>&1 || fail "release build failed" release.log
entry=$(python3 - <<'PY'
import json, glob
for db in glob.glob("target/**/compile_commands.json", recursive=True):
    for e in json.load(open(db)):
        if e["file"].replace("\\", "/").endswith("app/src/main.cpp"):
            args = e.get("arguments") or e["command"].split()
            if any("APP_RELEASE" in a for a in args):
                print(" ".join(args)); raise SystemExit
PY
)
case "$entry" in
    *-DWS_RELEASE=1*-DAPP_RELEASE=1*) ;;
    *) fail "[workspace.profile.release] and the member's [profile.release] did not merge, workspace first: $entry" release.log ;;
esac
echo "ok P3"

# ── P5 ────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/unknown/app" "$TMP/twice/app"
cp app/mcpp.toml "$TMP/unknown/app/"; cp app/mcpp.toml "$TMP/twice/app/"
printf '[workspace]\nmembers = ["app"]\n[workspace.tolchain]\ndefault = "gcc@16.1.0"\n' > "$TMP/unknown/mcpp.toml"
if ( cd "$TMP/unknown" && "$MCPP" build -p app > "$TMP/unknown.log" 2>&1 ); then
    fail "an unknown [workspace] table was accepted" "$TMP/unknown.log"
fi
grep -q "\[workspace\] has no key or table 'tolchain'" "$TMP/unknown.log" \
    || fail "the refusal does not name the table" "$TMP/unknown.log"
printf '[workspace]\nmembers = ["app"]\n[toolchain]\ndefault = "gcc@16.1.0"\n[workspace.toolchain]\ndefault = "gcc@16.1.0"\n' > "$TMP/twice/mcpp.toml"
if ( cd "$TMP/twice" && "$MCPP" build -p app > "$TMP/twice.log" 2>&1 ); then
    fail "one table written twice was accepted" "$TMP/twice.log"
fi
grep -q "write it once, as \[workspace.toolchain\]" "$TMP/twice.log" \
    || fail "the refusal does not say which spelling" "$TMP/twice.log"
echo "ok P5"

# ── P6 ────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/inert/app/src"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/inert/app/mcpp.toml"
echo 'int main() { return 0; }' > "$TMP/inert/app/src/main.cpp"
printf '[workspace]\nmembers = ["app"]\n[build]\ncxxflags = ["-DNOWHERE=1"]\n' > "$TMP/inert/mcpp.toml"
( cd "$TMP/inert" && "$MCPP" build -p app > "$TMP/inert.log" 2>&1 ) \
    || fail "W7 is a warning, not a refusal" "$TMP/inert.log"
grep -q "\[build\] is written on a workspace root without \[package\]" "$TMP/inert.log" \
    || fail "W7 said nothing" "$TMP/inert.log"
if ( cd "$TMP/inert" && "$MCPP" build -p app --strict > "$TMP/inert-strict.log" 2>&1 ); then
    fail "--strict accepted W7" "$TMP/inert-strict.log"
fi
echo "ok P6"

echo "PASS: 893_the_workspace_prefix_carries_the_meaning"
