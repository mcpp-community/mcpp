#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 949 -- a version the local index lacks is refreshed for when its install is
# refused, and the package the descriptor named is the only one asked for.
#
# The walk refreshes when a descriptor is missing, and deliberately not when a
# descriptor is present without the version asked for: version tables are per
# system, so that absence is not proof. The provider's "not found" for the
# version is. Measured: `openkal-llvm-runtime = "0.16.3"` against an index
# synced before 0.16.3 was refused until `mcpp index update`, after mcpp had
# also tried `compat:openkal-llvm-runtime` and `compat.openkal-llvm-runtime`
# and shown them as downloading.
#
# Hermetic, as 945 is: the builtin index is fabricated, and every case is
# debounced or has `auto_refresh = false`, so the decision is observed
# without a sync.
#
# Criteria:
#   A. The refused install reaches the refresh policy, which declines inside
#      the debounce window and says so for that address.
#   B. With `[index] auto_refresh = false` the same refusal is declined for
#      that reason.
#   C. A descriptor that was read names the package: no other namespace is
#      tried or shown as downloading, and the refusal names one address.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX="$MCPP_HOME/indices/mcpplibs"
mkdir -p "$INDEX/pkgs/l"
echo "abc1234" > "$INDEX/.xlings-index-version"
# The descriptor is here; the version asked for is not in it.
cat > "$INDEX/pkgs/l/laterpkg.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "laterpkg",
    description = "A package whose next version the local index predates",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/laterpkg-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
EOF

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
laterpkg = "1.0.1"
TOML
echo 'int main() { return 0; }' > src/main.cpp

# A
: > "$INDEX/.mcpp-index-updated"
MCPP_VERBOSE=1 "$MCPP" build > a.log 2>&1 && fail "A: the build installed a version no index carries" a.log
grep -qE "skip refresh for mcpplibs:laterpkg@1\.0\.1: index was just refreshed" a.log \
    || fail "A: the refused install did not reach the refresh policy" a.log
grep -qE "Refreshing +package index" a.log && fail "A: a sync inside the debounce window" a.log
echo "ok: a refused install asks for a refresh (declined inside the debounce window)"

# B
touch -d '30 days ago' "$INDEX/.mcpp-index-updated" 2>/dev/null \
    || touch -A -300000 "$INDEX/.mcpp-index-updated" 2>/dev/null || true
printf '\n[index]\nauto_refresh = false\n' >> "$MCPP_HOME/config.toml"
MCPP_VERBOSE=1 "$MCPP" build > b.log 2>&1 && fail "B: the build installed a version no index carries" b.log
grep -qE "skip refresh for mcpplibs:laterpkg@1\.0\.1: \[index\] auto_refresh = false" b.log \
    || fail "B: auto_refresh = false did not decide the refused install" b.log
echo "ok: with auto_refresh = false the refused install is declined"

# C
grep -qE "Downloading +compat" a.log b.log && fail "C: another namespace was shown as downloading" a.log
grep -q "wire address tried: mcpplibs:laterpkg@1.0.1$" a.log \
    || fail "C: the refusal does not name the one address asked for" a.log
echo "ok: the package the descriptor named is the only one asked for"

echo "PASS: 949 a version the index lacks refreshes at install"
