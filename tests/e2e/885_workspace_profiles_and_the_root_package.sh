#!/usr/bin/env bash
# requires: unix-shell
# 885 -- the workspace's `[profile.*]` and its own package (2026.10.5.2).
#
#   W1  a virtual root's `[profile.release]` reaches every member; it used to
#       be ignored without a word.
#   W2  a member's own table of a profile name replaces the workspace's.
#   W3  in a workspace with its own `[package]`, the root file's profile is the
#       same for every selection, so `--workspace` plans one graph and compiles
#       the library once, and the root package receives `[workspace.build]` as
#       every member does.
#
# Read from build.ninja: every statement carries a `-DMARK_*` word.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/mcpp-home"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF

graphs() { find target -name build.ninja | wc -l | tr -d ' '; }
# The compile edge of one source, with the file-level flags it inherits.
edge() {   # $1 = an object path fragment, e.g. obj/app/src/main
    local n; n=$(find target -name build.ninja | head -1)
    { grep -E '^(cxxflags|cflags) *=' "$n"
      awk -v o="$1" 'index($0, "build " o)==1 {f=1; print; next} f && /^  / {print; next} {f=0}' "$n"; }
}
profile() {   # a [profile.release] whose words name $1
    printf '[profile.release]\ncxxflags = ["-DMARK_PROFILE_%s"]\n' "$1"
}
member_lib() {
    mkdir -p "$1/src"
    printf '[package]\nname = "lib"\nversion = "0.1.0"\n\n[targets.lib]\nkind = "lib"\n' > "$1/mcpp.toml"
    printf 'export module t885_lib;\nexport int lv() { return 1; }\n' > "$1/src/lib.cppm"
}
app_main() {
    mkdir -p "$1/src"
    printf 'import t885_lib;\nint main() { return lv() == 1 ? 0 : 1; }\n' > "$1/src/main.cpp"
}

# ── W1 / W2 ────────────────────────────────────────────────────────────────
mkdir -p "$TMP/v" && cd "$TMP/v"
{ printf '[workspace]\nmembers = ["app", "lib"]\n\n'; profile WSROOT; } > mcpp.toml
member_lib lib
app_main app
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\nlib = { path = "../lib" }\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > app/mcpp.toml
"$MCPP" build --release -p app > w1.log 2>&1 || fail "W1: build failed" w1.log
edge obj/app/src/main > w1.edge
grep -q MARK_PROFILE_WSROOT w1.edge || fail "W1: the workspace's profile did not reach a member" w1.edge

{ cat app/mcpp.toml; echo; profile APP; } > app/mcpp.toml.new && mv app/mcpp.toml.new app/mcpp.toml
rm -rf target
"$MCPP" build --release -p app > w2.log 2>&1 || fail "W2: build failed" w2.log
edge obj/app/src/main > w2.edge
grep -q MARK_PROFILE_APP w2.edge || fail "W2: the member's own profile was not used" w2.edge
if grep -q MARK_PROFILE_WSROOT w2.edge; then
    fail "W2: the workspace's profile was merged into the member's own table" w2.edge
fi

# ── W3 ─────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/r" && cd "$TMP/r"
member_lib lib
app_main .
{ printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[build]\ncxxflags = ["-DMARK_OWN"]\n\n'
  printf '[dependencies]\nlib = { path = "lib" }\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n\n'
  printf '[workspace]\nmembers = ["lib"]\n\n[workspace.build]\ncxxflags = ["-DMARK_WS_BUILD"]\n\n'
  profile ROOTFILE; } > mcpp.toml
for selection in "" "--workspace"; do
    rm -rf target
    # shellcheck disable=SC2086
    "$MCPP" build --release $selection > w3.log 2>&1 || fail "W3: build ${selection:-of the root} failed" w3.log
    [ "$(graphs)" = 1 ] || fail "W3 (${selection:-root}): $(graphs) graphs, expected one" w3.log
    edge obj/app/src/main > w3.edge
    grep -q MARK_WS_BUILD w3.edge || fail "W3 (${selection:-root}): the root package did not receive [workspace.build]" w3.edge
    grep -q MARK_OWN w3.edge || fail "W3 (${selection:-root}): the root package lost its own [build]" w3.edge
    awk '{ if (gsub(/MARK_WS_BUILD/, "") > 1) bad = 1 } END { exit bad }' w3.edge \
        || fail "W3 (${selection:-root}): [workspace.build] was applied more than once" w3.edge
done
rm -rf target
"$MCPP" build --release -p lib > w3p.log 2>&1 || fail "W3: build -p lib failed" w3p.log
grep -rq MARK_PROFILE_ROOTFILE target/*/*/build.ninja \
    || fail "W3: the root file's profile did not reach a member built on its own" w3p.log

echo "PASS: 885 workspace profiles and the root package"
