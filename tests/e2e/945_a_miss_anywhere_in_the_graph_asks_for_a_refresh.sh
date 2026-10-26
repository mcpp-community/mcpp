#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 945 -- a request by version that the local index cannot answer asks for a
# refresh wherever it is in the graph, not only in the root's [dependencies].
#
# The refresh before the dependency walk (#315) asked about the root's and the
# selected members' [dependencies]; a package reached by `path` that named a
# version the index did not yet carry refused the build with no refresh at all.
# Measured in mcpp's own CI: modules/platform named `os = "0.1.0"` minutes
# after the index published it, and a sandbox index "never refreshed" answered
#
#   error: dependency 'mcpplibs.os': no package found for exact selector
#
# Hermetic, as 173 is: the builtin index is fabricated, and every case is
# offline or debounced, so the decision is observed without a sync.
#
# Criteria:
#   A. Offline, the miss in a path dependency reaches the refresh policy, which
#      declines for offline mode and says so for that package.
#   B. Inside the debounce window, the same miss is declined as "just
#      refreshed": the policy, not the position of the edge, decides.
#   C. The refusal that follows still names the selector and the index.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX="$MCPP_HOME/indices/mcpplibs"
mkdir -p "$INDEX/pkgs/w"
echo "abc1234" > "$INDEX/.xlings-index-version"
: > "$INDEX/.mcpp-index-updated"
touch -d '30 days ago' "$INDEX/.mcpp-index-updated" 2>/dev/null \
    || touch -A -300000 "$INDEX/.mcpp-index-updated" 2>/dev/null || true

# mid: reached by path; it names a package the local index does not carry.
mkdir -p "$TMP/mid/src"
cat > "$TMP/mid/mcpp.toml" <<'TOML'
[package]
name    = "mid"
version = "0.1.0"

[targets.mid]
kind = "lib"

[dependencies]
absentpkg = "1.0.0"
TOML
printf 'export module mid;\nexport int mid_value() { return 1; }\n' > "$TMP/mid/src/mid.cppm"

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
mid = { path = "../mid" }
TOML
echo 'int main() { return 0; }' > src/main.cpp

# A
MCPP_VERBOSE=1 MCPP_OFFLINE=1 "$MCPP" build > a.log 2>&1 && fail "A: the build resolved a package no index carries" a.log
grep -qE "skip refresh for (mcpplibs[.:])?absentpkg: offline mode" a.log \
    || fail "A: the miss in a path dependency did not reach the refresh policy" a.log
echo "ok: a miss in a path dependency asks for a refresh (declined offline)"

# B
: > "$INDEX/.mcpp-index-updated"
MCPP_VERBOSE=1 "$MCPP" build > b.log 2>&1 && fail "B: the build resolved a package no index carries" b.log
grep -qE "skip refresh for (mcpplibs[.:])?absentpkg: index was just refreshed" b.log \
    || fail "B: the debounce did not decide the miss in a path dependency" b.log
grep -qE "Refreshing +package index" b.log && fail "B: a sync inside the debounce window" b.log
echo "ok: inside the debounce window the same miss is declined"

# C
grep -q "dependency 'absentpkg': no package found for exact selector" b.log \
    || fail "C: the refusal does not name the selector" b.log
grep -q "index: local index abc1234" b.log || fail "C: the refusal does not name the index" b.log
echo "ok: the refusal names the selector and the index"

echo "PASS: 945 a miss anywhere in the graph asks for a refresh"
