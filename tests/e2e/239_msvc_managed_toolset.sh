#!/usr/bin/env bash
# requires: msvc
# 97_msvc_managed_toolset.sh — `msvc@<toolset>`: the toolset the manifest
# names is the one that compiles, regardless of what this machine has.
#
# WHY THIS TEST CANNOT BE SATISFIED BY THE MACHINE'S COMPILER, which is the
# only reason it is worth running: the runner has its own Visual Studio, and
# every assertion below is written so that the system install answering
# instead would FAIL rather than pass quietly.
#
#   - the resolved cl.exe must live under mcpp's payload store
#   - its toolset directory must be the version the spec named
#   - `msvc@system` on the same machine must still resolve to the SYSTEM cl,
#     i.e. the two origins do not contaminate each other
#
# Network: installs xim:msvc (~85 MB) + xim:windows-sdk (~135 MB). Skips
# cleanly when the index cannot be reached, because an offline runner has
# nothing to say about this.
set -e

TOOLSET="14.44.35207"       # release channel; `latest` in xim:msvc

CONF="${MCPP_HOME:-$HOME/.mcpp}/config.toml"
ORIG_DEFAULT=""
if [[ -f "$CONF" ]]; then
    ORIG_DEFAULT=$(sed -n '/^\[toolchain\]/,/^\[/p' "$CONF" \
        | grep -E '^default[[:space:]]*=' | head -1 | cut -d'"' -f2 || true)
fi
TMP=$(mktemp -d)
restore() {
    if [[ -n "$ORIG_DEFAULT" ]]; then
        "$MCPP" toolchain default "$ORIG_DEFAULT" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap restore EXIT
cd "$TMP"

# 0) Decide SKIP here, from a positive check, and never again.
#
#    ⚠️ This used to be decided AFTER the install by pattern-matching the
#    failure text, and one of the patterns was `*"index"*`. Nearly every mcpp
#    command prints "package index" somewhere, so every genuine install
#    failure took the skip branch: the test could pass or skip, never fail.
#
#    It hid a real one. `tar -xf "C:\...vsix"` fails under GNU tar, which
#    reads `C:` as a hostname ("Cannot connect to C: resolve failed"); the
#    install ran for 135 seconds, failed, and this script reported PASS.
#
#    A skip has to be decided by what is ABSENT before the work starts, not by
#    what the failure looked like afterwards.
if ! "$MCPP" toolchain list 2>&1 | grep -qi "msvc"; then
    echo "SKIP: this index offers no msvc toolset (offline runner?)"
    exit 0
fi

# 1) install it. Any failure from here on is a FAILURE.
rc=0; out=$("$MCPP" toolchain install msvc "$TOOLSET" 2>&1) || rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: install msvc $TOOLSET (rc=$rc):"
    echo "$out"
    exit 1
fi
[[ "$out" == *"$TOOLSET"* ]] \
    || { echo "FAIL: install did not report the toolset: $out"; exit 1; }

# 1b) it must now be LISTED. A toolset that installs but never appears is
#     indistinguishable from one that did not install, and `toolchain list` is
#     where a user looks.
out=$("$MCPP" toolchain list 2>&1)
[[ "$out" == *"msvc"* && "$out" == *"$TOOLSET"* ]] \
    || { echo "FAIL: installed toolset absent from toolchain list: $out"; exit 1; }

# 2) build with it, from a manifest — the path that matters, and the one
#    where the version used to be accepted and then ignored.
"$MCPP" new hello_pinned >/dev/null 2>&1
cd hello_pinned
cat >> mcpp.toml <<EOF

[toolchain]
windows = "msvc@$TOOLSET"
EOF

out=$("$MCPP" build --verbose 2>&1) || { echo "FAIL: pinned build: $out"; exit 1; }

# The resolved compiler must be the PAYLOAD's, not the machine's. Two
# independent facts, because either alone can be true by accident: the path
# is inside mcpp's store, AND the toolset directory is the one named.
resolved=$(echo "$out" | grep -iE "Resolved .*msvc" | head -1)
[[ -n "$resolved" ]] || { echo "FAIL: no Resolved line: $out"; exit 1; }
case "$resolved" in
    *xpkgs*xim-x-msvc*) ;;
    *) echo "FAIL: cl.exe is not from mcpp's store: $resolved"; exit 1 ;;
esac
case "$resolved" in
    *"$TOOLSET"*) ;;
    *) echo "FAIL: resolved toolset is not $TOOLSET: $resolved"; exit 1 ;;
esac

out=$("$MCPP" run 2>&1) || { echo "FAIL: pinned run: $out"; exit 1; }
[[ "$out" == *"Hello"* || "$out" == *"hello"* ]] \
    || { echo "FAIL: hello output: $out"; exit 1; }

# 3) THE REVERSE DIRECTION. Same machine, same project, spec switched back to
#    msvc@system: it must resolve to the SYSTEM cl again. Without this, a
#    "managed works" result is equally consistent with "managed replaced
#    everything", and the system origin would be quietly gone.
sed -i "s|windows = \"msvc@$TOOLSET\"|windows = \"msvc@system\"|" mcpp.toml
out=$("$MCPP" build --verbose 2>&1) || { echo "FAIL: system build: $out"; exit 1; }
resolved=$(echo "$out" | grep -iE "Resolved .*msvc" | head -1)
[[ -n "$resolved" ]] || { echo "FAIL: no Resolved line (system): $out"; exit 1; }
case "$resolved" in
    *xpkgs*xim-x-msvc*)
        echo "FAIL: msvc@system resolved to the PAYLOAD — the origins leak: $resolved"
        exit 1 ;;
esac

# 4) a toolset mcpp installed is removable — the other half of the message
#    `toolchain remove msvc` prints.
cd "$TMP"
out=$("$MCPP" toolchain remove "msvc@$TOOLSET" 2>&1) \
    || { echo "FAIL: remove pinned toolset: $out"; exit 1; }
[[ "$out" == *"Removed"* ]] || { echo "FAIL: remove message: $out"; exit 1; }

echo "PASS: msvc@$TOOLSET installs, builds, stays distinct from msvc@system, and removes"
