#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# #315: `mcpp build` must not refresh the package index just because time
# passed. It refreshes when — and only when — the local index cannot answer.
#
# Every case here is HERMETIC: the builtin registry is fabricated on disk and
# every path that could reach the network is either offline-gated or debounced,
# so this test never downloads anything and never depends on upstream state.
#
# The load-bearing case is INV-3 (step 3 below): a namespace the registry cannot
# refute — `xim`, whose descriptors declare no namespace, or any third-party ns
# — must NOT count as a miss. If it did, every build with such a dependency
# would sync EVERY TIME, which is strictly worse than the hourly TTL this
# change removes.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX="$MCPP_HOME/registry/data/mcpplibs"
mkdir -p "$INDEX/pkgs/w"

cat > "$INDEX/pkgs/w/widget.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "widget",
    description = "Present in the local index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.2.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.2.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.2.0"] = { url = "https://example.invalid/w.zip",    sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
}
EOF
echo "abc1234" > "$INDEX/.xlings-index-version"

# Back-date the refresh marker well past any debounce window: under the old
# TTL policy this alone was enough to trigger a network sync on every build.
: > "$INDEX/.mcpp-index-updated"
touch -d '30 days ago' "$INDEX/.mcpp-index-updated" 2>/dev/null \
    || touch -A -300000 "$INDEX/.mcpp-index-updated" 2>/dev/null || true

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
echo 'int main() { return 0; }' > src/main.cpp

manifest() {   # manifest <dependency-lines...>
    cat > mcpp.toml <<EOF
[package]
name    = "proj"
version = "0.1.0"

[dependencies]
$1

[targets.proj]
kind = "bin"
main = "src/main.cpp"
EOF
}

# NOTE on assertions: `! $MCPP build | grep -q X` would be useless here — the
# left side of a pipe is exempt from errexit, so such a check can never fail.
# Capture, then test the captured text.
refute() {   # refute <log> <pattern> <message>
    if grep -qE "$2" <<< "$1"; then
        printf '%s\n' "$1"
        echo "FAIL: $3"
        exit 1
    fi
}
expect() {   # expect <log> <pattern> <message>
    if ! grep -qE "$2" <<< "$1"; then
        printf '%s\n' "$1"
        echo "FAIL: $3"
        exit 1
    fi
}

# ── 1. Steady state: a resolvable dependency costs no network ─────────────
# The descriptor is present and the constraint is satisfiable, so the decision
# is "resolvable locally" no matter how old the marker is. The build is run
# offline so that a regression here surfaces as a missing/extra decision line
# rather than as a download.
manifest 'widget = "1.2.0"'
log=$(MCPP_VERBOSE=1 MCPP_OFFLINE=1 "$MCPP" build 2>&1 || true)
expect "$log" 'widget@1.2.0: resolvable locally' \
    'a locally-resolvable dependency should be reported as such'
refute "$log" 'Refreshing +package index|Updating +package index' \
    'a 30-day-old marker must NOT trigger a refresh when the index can answer'

# A SemVer constraint that the local versions satisfy is equally steady.
manifest 'widget = "^1.2"'
log=$(MCPP_VERBOSE=1 MCPP_OFFLINE=1 "$MCPP" build 2>&1 || true)
expect "$log" 'widget@\^1.2: resolvable locally' \
    'a satisfiable constraint should not be a version miss'
refute "$log" 'Refreshing +package index' \
    'a satisfiable constraint must not trigger a refresh'

# ── 2. A real miss is reported as one, and offline suppresses the sync ────
manifest 'absentpkg = "1.0.0"'
log=$(MCPP_VERBOSE=1 MCPP_OFFLINE=1 "$MCPP" build 2>&1 || true)
expect "$log" 'absentpkg@1.0.0: offline mode' \
    'offline mode must suppress the refresh and say so'
refute "$log" 'Refreshing +package index' \
    'offline mode must not reach the network'
# The failure the user actually sees carries the index identity and age, so
# "not found" and "your index is from last month" are distinguishable.
expect "$log" 'index: local index abc1234' \
    'the resolution failure should name the index revision'
expect "$log" 'mcpp index update' \
    'the resolution failure should point at the explicit refresh command'

# ── 3. INV-3: an unrefutable miss is not a miss ───────────────────────────
# Neither of these may trigger a refresh. `xim` descriptors declare no
# namespace so `(xim, nasm)` can never match the identity gate, and a
# third-party namespace is outside the identities mcpp defines at all.
for dep in 'xim.nasm = "2.16.03"' 'somevendor.thing = "1.0.0"'; do
    manifest "$dep"
    log=$(MCPP_VERBOSE=1 "$MCPP" build 2>&1 || true)     # deliberately NOT offline
    expect "$log" 'no index can refute this' \
        "a miss under an unrefutable namespace must be inconclusive: $dep"
    refute "$log" 'Refreshing +package index' \
        "an unrefutable miss must never trigger a refresh: $dep"
done

# ── 4. Debounce: a second miss inside the window does not re-sync ─────────
: > "$INDEX/.mcpp-index-updated"      # "just refreshed"
manifest 'absentpkg = "1.0.0"'
log=$(MCPP_VERBOSE=1 "$MCPP" build 2>&1 || true)          # deliberately NOT offline
expect "$log" 'index was just refreshed' \
    'a miss moments after a sync must be debounced, not re-synced'
refute "$log" 'Refreshing +package index' \
    'debounce must prevent the second sync'

# ── 5. [index] auto_refresh = false is honoured ───────────────────────────
touch -d '30 days ago' "$INDEX/.mcpp-index-updated" 2>/dev/null \
    || touch -A -300000 "$INDEX/.mcpp-index-updated" 2>/dev/null || true
printf '\n[index]\nauto_refresh = false\n' >> "$MCPP_HOME/config.toml"
log=$(MCPP_VERBOSE=1 "$MCPP" build 2>&1 || true)          # deliberately NOT offline
expect "$log" 'auto_refresh = false' \
    'the config opt-out must suppress the refresh and name itself'
refute "$log" 'Refreshing +package index' \
    'auto_refresh = false must prevent the sync'

# ── 6. `mcpp index update` refuses offline instead of silently no-opping ──
log=$(MCPP_OFFLINE=1 "$MCPP" index update 2>&1 || true)
expect "$log" 'offline mode is on' \
    'an explicit index update must refuse audibly when offline'

# ── 7. `mcpp index status` reports the index revision ─────────────────────
log=$(MCPP_OFFLINE=1 "$MCPP" index status 2>&1 || true)
expect "$log" 'revision' 'index status should have a revision column'
expect "$log" 'abc1234'  'index status should show the local index revision'

echo "PASS: index refresh is driven by local resolvability, not by the clock"
