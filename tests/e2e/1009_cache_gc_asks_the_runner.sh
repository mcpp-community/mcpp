#!/usr/bin/env bash
# requires: unix-shell
# 1009 -- `mcpp cache gc` asks the newest okrun among the host tools it built
# to reclaim the runner's own cache (`okrun cache gc`, openkal-image 0.4;
# design 2026-11-03 part 4 §16; 2026.11.3.1).
#
# The prepared copies and snapshots an ok image's runner keeps are laid out by
# the runner, so the runner reclaims them. Two openkal-tools entries stand in
# here: the older one must not be asked, the newer one is asked `cache gc` and
# what it says is reported. A home with no okrun asks nothing.
set -e
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
tools="$MCPP_HOME/build-cache/v1/tool/mcpplibs"

# A home that built no okrun: gc collects as before and asks no runner.
mkdir -p "$MCPP_HOME/build-cache/v1/tool"
"$MCPP" cache gc --older-than 3650d > none.log 2>&1 || { cat none.log; exit 1; }
if grep -q 'Reclaimed' none.log; then
    echo "FAIL: a home without okrun reported a runner's cache"; cat none.log; exit 1
fi

runner() {   # runner <version> <key> <what it says>
    local bin="$tools/openkal-tools@$1/$2/bin"
    mkdir -p "$bin"
    cat > "$bin/okrun" <<SH
#!/bin/sh
echo "\$@" > "$TMP/asked-$1"
echo "$3" >&2
SH
    chmod +x "$bin/okrun"
}
runner 0.3.2 aaaa "okrun: the older runner"
runner 0.4.1 bbbb "okrun: removed 7 bytes prepared for another boot"
runner 0.4.0 cccc "okrun: the middle runner"
# Numeric version order, and cache entries that cannot name a released version.
runner 0.10.1 dddd "okrun: removed 9 bytes prepared for another boot"
runner 0.99broken eeee "okrun: not a version"
runner 999999999999999999999999 ffff "okrun: overflowing version"

"$MCPP" cache gc --older-than 3650d > gc.log 2>&1 || { cat gc.log; exit 1; }
[[ "$(cat "$TMP/asked-0.10.1" 2>/dev/null)" == "cache gc" ]] || {
    echo "FAIL: the newest okrun was not asked \`cache gc\`"; cat gc.log; exit 1
}
[[ ! -e "$TMP/asked-0.3.2" && ! -e "$TMP/asked-0.4.0" && ! -e "$TMP/asked-0.4.1" ]] || {
    echo "FAIL: an older okrun was asked"; cat gc.log; exit 1
}
[[ ! -e "$TMP/asked-0.99broken" && ! -e "$TMP/asked-999999999999999999999999" ]] || {
    echo "FAIL: an invalid version was asked"; cat gc.log; exit 1
}
grep -q 'Reclaimed.*removed 9 bytes prepared for another boot' gc.log || {
    echo "FAIL: what the runner reclaimed is not reported"; cat gc.log; exit 1
}
echo "OK"
