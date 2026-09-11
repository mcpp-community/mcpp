#!/usr/bin/env bash
# requires: gcc
# 641_the_android_rows_are_wired_and_the_simulator_is_a_row.sh — the vocabulary
# half of the Android and iOS work, which is the half a runner without a 704 MB
# NDK can still assert. Nothing here installs a payload.
#
# WHAT THIS FILE DELIBERATELY DOES NOT ASSERT: that an Android artefact builds.
# That was measured by hand (linux-x86_64, xim:android-ndk 30.0.16248370, a
# source that imports std and no project vocabulary beyond `--target`):
#
#   aarch64-linux-android -> ELF 64-bit LSB pie, ARM aarch64,
#                            interpreter /system/bin/linker64
#   x86_64-linux-android  -> ELF 64-bit LSB pie, x86-64, same interpreter
#
# and it is what the rows' `preview` tier records. An e2e that needed the
# payload would skip on every shard, which this repository has already paid for
# once: `# requires: llvm` tests ran on no CI job at all while reporting green.
set -e

t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail=0

pkg() {  # dir  [extra manifest lines...]
    local d=$1; shift
    mkdir -p "$d/src"
    printf '[package]\nname = "c"\nversion = "0.1.0"\n' > "$d/mcpp.toml"
    for line in "$@"; do printf '%s\n' "$line" >> "$d/mcpp.toml"; done
    printf 'int main(){return 0;}\n' > "$d/src/main.cpp"
}

echo "== 641: the rows are wired, and a spelling that exists is not 'unknown' =="

# 1. BOTH ANDROID ROWS NAME THEIR PAYLOAD. A row whose tier moved without a pin
#    is reachable only through an explicit `[target.X] toolchain` override,
#    which is the escape hatch rather than the support claim.
list=$( "$MCPP" toolchain list --format json 2>/dev/null )
for target in aarch64-linux-android x86_64-linux-android; do
    if grep -q "\"target\": *\"$target\"" <<<"$list" \
       && tr ',' '\n' <<<"$list" | grep -A6 "\"target\": *\"$target\"" \
          | grep -q "android-ndk"; then
        echo "  ok: $target names the NDK payload"
    else
        echo "FAIL: $target does not name android-ndk in toolchain list"
        tr ',' '\n' <<<"$list" | grep -A6 "\"target\": *\"$target\"" | sed 's/^/    /'
        fail=1
    fi
done

# 2. AND THEY ARE NO LONGER `planned`. This is the property the user's question
#    was about, and it is separate from (1): a row can carry a pin and still be
#    refused by the tier gate.
for target in aarch64-linux-android x86_64-linux-android; do
    d="$t/tier-$target"; pkg "$d"
    out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target "$target" 2>&1 ) || true
    if grep -q "not yet supported (planned)" <<<"$out"; then
        echo "FAIL: $target is still refused as planned"
        fail=1
    else
        echo "  ok: $target is not refused for its tier"
    fi
done

# 3. THE SIMULATOR IS A ROW, so its spelling resolves. Before it existed,
#    `--target aarch64-ios-sim` answered `unknown target`, which was false: the
#    vocabulary has the device row and the simulator is a different target, not
#    an unspellable one.
for target in aarch64-ios-sim x86_64-ios-sim; do
    d="$t/sim-$target"; pkg "$d"
    out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target "$target" 2>&1 ) || true
    if grep -q "unknown target" <<<"$out"; then
        echo "FAIL: $target reported as unknown; it is a registered row"
        fail=1
    elif grep -q "not yet supported (planned)" <<<"$out"; then
        echo "  ok: $target says planned, naming the row"
    else
        echo "FAIL: $target refused for neither reason"
        grep -m2 -E "^error" <<<"$out" | sed 's/^/    /'
        fail=1
    fi
done

# 4. AN EFFECTIVE TRIPLE mcpp PRINTS ITSELF MUST PARSE BACK.
#    `Target aarch64-linux-android -> aarch64-unknown-linux-android21` is a
#    line mcpp writes; pasting it back used to answer `unknown target`, because
#    the env match read `k == "android"` and the API level rides that segment.
d="$t/effective"; pkg "$d"
out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target aarch64-unknown-linux-android21 2>&1 ) || true
if grep -q "unknown target" <<<"$out"; then
    echo "FAIL: the effective triple mcpp prints does not parse back"
    grep -m2 -E "^error" <<<"$out" | sed 's/^/    /'
    fail=1
else
    echo "  ok: aarch64-unknown-linux-android21 parses to the canonical row"
fi

# 5. A BARE `aarch64-linux` IS NEVER COMPLETED TO ANDROID. The rows sit on the
#    same `arch-os` prefix because the kernel IS Linux, and that is the whole
#    reason every Linux-shaped answer in the tree is right about them. It does
#    not make bionic a candidate C library for a request that named none.
d="$t/bare"; pkg "$d"
out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target aarch64-linux 2>&1 ) || true
if grep -q "android" <<<"$out"; then
    echo "FAIL: a bare aarch64-linux request mentioned android"
    grep -m4 -E "android" <<<"$out" | sed 's/^/    /'
    fail=1
else
    echo "  ok: a bare aarch64-linux request never mentions android"
fi

# 6. `min_api_level` IS A MANIFEST KEY WITH A FLOOR, and it is refused where a
#    reader can act on it. Manifest-level, so no payload is needed: the check
#    runs before any toolchain is resolved.
d="$t/apilevel"; pkg "$d" "" "[target.aarch64-linux-android]" "min_api_level = 0"
out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target aarch64-linux-android 2>&1 ) || true
if grep -q "min_api_level must be a positive integer" <<<"$out"; then
    echo "  ok: a non-positive min_api_level is refused naming the key"
else
    echo "FAIL: min_api_level = 0 was not refused with its own message"
    grep -m3 -E "^error" <<<"$out" | sed 's/^/    /'
    fail=1
fi

# And a legal one is accepted as far as the manifest is concerned -- the build
# may then fail for want of a payload, which is a different sentence.
d="$t/apilevel-ok"; pkg "$d" "" "[target.aarch64-linux-android]" "min_api_level = 24"
out=$( cd "$d" && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target aarch64-linux-android 2>&1 ) || true
if grep -q "min_api_level must be" <<<"$out"; then
    echo "FAIL: a legal min_api_level = 24 was refused by the manifest"
    fail=1
else
    echo "  ok: min_api_level = 24 is accepted by the manifest"
fi

if [ "$fail" -ne 0 ]; then echo "FAIL: 641"; exit 1; fi
echo "PASS: 641"
