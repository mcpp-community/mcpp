#!/usr/bin/env bash
# requires: elf gcc android-ndk
# 680 -- the engine states the target's platform floor as a fact in the
# platform's own words (`android.api-level`), and a dependency refuses a floor
# below what it needs through the ordinary `version-floor` requirement
# (#634, A9). The floor is not raised for the dependency: the application's
# floor decides which devices it installs on.
#
# Legs, on the `x86_64-linux-android` row:
#   A. A root that states no level: refused, naming where 21 came from.
#   B. A root stating 21: refused, naming `[target.<triple>] min_api_level`,
#      and saying "this build targets".
#   C. A root stating 24: builds.
#   D. The host row states no such fact, so the requirement is silent there.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

TARGET=x86_64-linux-android
mkdir -p "$TMP/fw/src" "$TMP/app/src"
cat > "$TMP/fw/mcpp.toml" <<'TOML'
[package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
[[runtime.requirements]]
kind  = "version-floor"
value = "android.api-level >= 23"
TOML
printf 'export module fw;\nexport int fw_anchor() { return 41; }\n' > "$TMP/fw/src/fw.cppm"
printf 'import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n' > "$TMP/app/src/main.cpp"
base='[package]
name    = "app"
version = "0.1.0"
[dependencies]
demo.fw = { path = "../fw" }
'
cd "$TMP/app"

# ── A ──────────────────────────────────────────────────────────────────────
printf '%s' "$base" > mcpp.toml
if "$MCPP" build --target "$TARGET" > a.log 2>&1; then fail "A: an unset level below the floor built" a.log; fi
grep -q "requires android.api-level >= 23, and this build targets" a.log || fail "A: refusal wording" a.log
grep -q "min_api_level is not set" a.log || fail "A: the refusal does not say where the level came from" a.log

# ── B ──────────────────────────────────────────────────────────────────────
printf '%s[target.%s]\nmin_api_level = 21\n' "$base" "$TARGET" > mcpp.toml
if "$MCPP" build --target "$TARGET" > b.log 2>&1; then fail "B: level 21 built" b.log; fi
grep -q "requires android.api-level >= 23, and this build targets 21" b.log || fail "B: refusal wording" b.log
grep -qF "set by: [target.$TARGET] min_api_level" b.log || fail "B: the refusal does not name the key" b.log

# ── C ──────────────────────────────────────────────────────────────────────
printf '%s[target.%s]\nmin_api_level = 24\n' "$base" "$TARGET" > mcpp.toml
"$MCPP" build --target "$TARGET" > c.log 2>&1 || fail "C: level 24 was refused" c.log

# ── D ──────────────────────────────────────────────────────────────────────
printf '%s' "$base" > mcpp.toml
"$MCPP" build > d.log 2>&1 || fail "D: the host row was refused" d.log
if grep -q "android.api-level" d.log; then fail "D: the host row mentions the Android floor" d.log; fi

echo "PASS: 680_a_dependency_floor_on_the_android_api_level"
