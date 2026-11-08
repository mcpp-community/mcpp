#!/usr/bin/env bash
# requires: python3
# 1029 -- an unused std module is collected by age (mcpp#874).
#
# The build cache keeps one std module per toolchain and flag set. After a build
# an entry of any kind unused for `[cache] unused_days` is collected, std
# modules included; `mcpp cache prune --older-than` and `cache gc --older-than`
# skipped them, so 159 std builds (4.9 GiB) survived a prune of two days.
#
# Criteria:
#   A. `cache prune --older-than 1d` removes a std module unused for 3 days and
#      keeps one used now.
#   B. `cache gc --older-than 1d` does the same.
#   C. `cache gc --max-size 0` keeps std modules: a size budget does not evict
#      what costs half a minute to build again.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/home"
std="$MCPP_HOME/build-cache/v1/std"

seed() {   # $1 name, $2 days since last use
    mkdir -p "$std/$1/pcm.cache"
    printf '{"compiler":"gcc","compiler_version":"16.1.0","cpp_standard":"c++23","stdlib":"libstdc++"}\n' > "$std/$1/std-module.json"
    printf 'x' > "$std/$1/std.o"
    python3 - "$std/$1" "$2" <<'PY'
import os, sys, time
t = time.time() - float(sys.argv[2]) * 86400
for root, dirs, files in os.walk(sys.argv[1]):
    for n in files: os.utime(os.path.join(root, n), (t, t))
    os.utime(root, (t, t))
PY
}

for cmd in "cache prune --older-than 1d" "cache gc --older-than 1d"; do
    rm -rf "$std"; seed old 3; seed fresh 0
    # shellcheck disable=SC2086
    "$MCPP" $cmd > c.log 2>&1 || fail "mcpp $cmd" c.log
    [ ! -e "$std/old" ] || fail "mcpp $cmd kept a std module unused for 3 days" c.log
    [ -e "$std/fresh" ] || fail "mcpp $cmd removed a std module used now" c.log
    echo "ok: mcpp $cmd"
done

rm -rf "$std"; seed old 3
"$MCPP" cache gc --max-size 0 > c.log 2>&1 || fail "mcpp cache gc --max-size 0" c.log
[ -e "$std/old" ] || fail "a size budget evicted a std module" c.log
echo "ok: a size budget keeps std modules"

echo "PASS: 1029 an unused std module is collected by age"
