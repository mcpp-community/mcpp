#!/usr/bin/env bash
# requires: gcc
# 894 -- COMPAT(workspace-position): a workspace root's own `[profile.*]`,
# `[toolchain]`, `[indices]`, the rows of `[target.<t>]` and `[xlings]` still
# reach its members by POSITION, with the merge they had, until mcpp 1.0.0
# (SPEC-004 §9.10, design 2026-10-10 §3.6). What changed is that it is said.
#
#   C1  A root without `[package]`: the root's `[profile.release]` reaches the
#       member as before, and every use is reported with the canonical
#       spelling.
#   C2  A member's own table of that name replaces the root's whole, as before.
#   C3  A root with `[package]`: building only the root package says nothing --
#       the table is that package's own -- and building a member that receives
#       it by position names the member.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

flags_of() {  # $1 = path suffix of the source; every argument of its entry, one line
    python3 - "$1" <<'PY'
import json, glob, sys
for db in glob.glob("target/**/compile_commands.json", recursive=True):
    for e in json.load(open(db)):
        if e["file"].replace("\\", "/").endswith(sys.argv[1]):
            print(" ".join(e.get("arguments") or e["command"].split()))
PY
}

member() {  # $1 = dir, $2 = extra manifest text
    mkdir -p "$1/src"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n%s\n' \
        "$(basename "$1")" "$(basename "$1")" "$2" > "$1/mcpp.toml"
    echo 'int main() { return 0; }' > "$1/src/main.cpp"
}

# ── C1, C2 ────────────────────────────────────────────────────────────────
mkdir -p "$TMP/virtual" && cd "$TMP/virtual"
printf '[workspace]\nmembers = ["plain", "own"]\n\n[profile.release]\ncxxflags = ["-DROOT_RELEASE=1"]\n' > mcpp.toml
member plain ""
member own $'[profile.release]\ncxxflags = ["-DOWN_RELEASE=1"]'
"$MCPP" build -p plain --release > plain.log 2>&1 || fail "plain" plain.log
flags_of plain/src/main.cpp | grep -q -- "-DROOT_RELEASE=1" \
    || fail "the root's [profile.release] no longer reaches the member by position" plain.log
grep -q "\[profile.<name>\] on the workspace root reaches every member by its position" plain.log \
    || fail "the position reading said nothing" plain.log
grep -q "\[workspace.profile.<name>\]" plain.log || fail "the warning names no canonical spelling" plain.log
"$MCPP" build -p own --release > own.log 2>&1 || fail "own" own.log
got=$(flags_of own/src/main.cpp)
case "$got" in
    *-DOWN_RELEASE=1*) ;;
    *) fail "the member's own [profile.release] was lost: $got" own.log ;;
esac
case "$got" in
    *-DROOT_RELEASE=1*) fail "the position reading merged a named table it used to replace whole: $got" own.log ;;
esac
echo "ok C1 C2"

# ── C3 ────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/rooted/src" && cd "$TMP/rooted"
printf '[package]\nname = "root"\nversion = "0.1.0"\n[targets.root]\nkind = "bin"\nmain = "src/main.cpp"\n\n[workspace]\nmembers = ["sub"]\n\n[profile.release]\ncxxflags = ["-DROOT_RELEASE=1"]\n' > mcpp.toml
echo 'int main() { return 0; }' > src/main.cpp
member sub ""
"$MCPP" build --release > root.log 2>&1 || fail "root package" root.log
if grep -q "workspace-position\|by its position" root.log; then
    fail "the root package's own table was reported as a position reading" root.log
fi
"$MCPP" build -p sub --release > sub.log 2>&1 || fail "sub" sub.log
grep -q "reaches member 'sub' by its position" sub.log \
    || fail "a member that received the root's table by position was not told" sub.log
flags_of sub/src/main.cpp | grep -q -- "-DROOT_RELEASE=1" \
    || fail "the position reading changed for a rooted workspace" sub.log
echo "ok C3"

echo "PASS: 894_the_position_reading_is_kept_and_reported"
