#!/usr/bin/env bash
#
# Version / pin drift guard.
#
# Two invariants that used to live only in a comment:
#
#   1. Every xlings version pinned anywhere in .github/ equals
#      `pinned::kXlingsVersion` in src/xlings.cppm — which is the version
#      `mcpp self env` reports and the one release.yml bundles into the
#      tarball as <install>/registry/bin/xlings.
#
#   2. mcpp's own version is identical in all four places that carry it.
#
# Why this exists: src/xlings.cppm used to say "keep in lock-step with the
# XLINGS_VERSION pins in release.yml / cross-build-test.yml / ci-linux-e2e.yml"
# and that list was ALREADY incomplete — it omitted both composite actions,
# which sat on 0.4.30 while everything else moved to 0.4.69. CI's sandbox
# silently rotted for weeks. A comment cannot enforce a cross-file invariant;
# this can.
#
# Deliberately pure text extraction with no mcpp dependency: the guard has to
# run when the build is broken, which is precisely when pins are being changed.
#
# Usage: bash .github/tools/check_version_pins.sh [repo_dir]

set -uo pipefail

REPO_DIR="${1:-$(pwd)}"
cd "$REPO_DIR" || { echo "FAIL: cannot cd to $REPO_DIR" >&2; exit 1; }

fail=0
note() { printf '%s\n' "$*" >&2; }
bad()  { printf 'FAIL: %s\n' "$*" >&2; fail=1; }

# Strip YAML/shell comments so historical references in prose ("before 0.4.69
# the index keyed by bare name") are not mistaken for live pins.
strip_comments() { sed 's/#.*//'; }

# ── 1. xlings pins ────────────────────────────────────────────────────────

XLINGS_EXPECTED=$(grep -oE 'kXlingsVersion[[:space:]]*=[[:space:]]*"[^"]+"' src/xlings.cppm \
                  | grep -oE '"[^"]+"' | tr -d '"' | head -1)
[ -n "$XLINGS_EXPECTED" ] || {
  echo "FAIL: could not read kXlingsVersion from src/xlings.cppm" >&2; exit 1; }

note "expected xlings pin: $XLINGS_EXPECTED  (src/xlings.cppm)"

# Anchored patterns only — a bare "version-looking number on a line mentioning
# xlings" would also match `xlings install llvm@20.1.7`, which pins LLVM, not
# xlings. Each alternative below ties the number to xlings itself.
#   XLINGS_VERSION: '<v>'                 workflow env
#   default: '<v>'                        composite action input (handled below)
#   xlings-<v>-<platform>                 tarball / extracted dir name
#   xlings/releases/download/v<v>         direct release URL
#   quick_install.sh … bash -s v<v>       bootstrap installer
#   xlings@<v>                            xim target
scan_xlings_pins() {
  local f="$1"
  strip_comments < "$f" | grep -nE \
    "XLINGS_VERSION:[[:space:]]*'?[0-9]|xlings-[0-9]+\.[0-9]|xlings/releases/download/v[0-9]|bash -s v[0-9]|xlings@[0-9]" \
    | while IFS= read -r line; do
        local no="${line%%:*}"
        local ver
        ver=$(printf '%s' "$line" | grep -oE \
              "(XLINGS_VERSION:[[:space:]]*'?|xlings-|download/v|bash -s v|xlings@)[0-9]+(\.[0-9]+)+" \
              | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
        [ -n "$ver" ] && printf '%s:%s\t%s\n' "$f" "$no" "$ver"
      done
}

# The composite actions carry the pin as the `default:` of an input named
# `xlings-version`, several lines below the input key — so match it by block,
# not by line.
scan_action_default() {
  local f="$1"
  strip_comments < "$f" | awk -v file="$f" '
    /^[[:space:]]*xlings-version:[[:space:]]*$/ { inblock = 1; next }
    inblock && /^[[:space:]]*[a-zA-Z_-]+:[[:space:]]*$/ && !/default/ { inblock = 0 }
    inblock && /^[[:space:]]*default:/ {
      if (match($0, /[0-9]+(\.[0-9]+)+/))
        printf "%s:%d\t%s\n", file, NR, substr($0, RSTART, RLENGTH)
      inblock = 0
    }'
}

found_any=0
while IFS= read -r f; do
  [ -f "$f" ] || continue
  while IFS=$'\t' read -r loc ver; do
    [ -n "${ver:-}" ] || continue
    found_any=1
    if [ "$ver" != "$XLINGS_EXPECTED" ]; then
      bad "$loc pins xlings $ver, expected $XLINGS_EXPECTED"
    fi
  done < <( { scan_xlings_pins "$f"; case "$f" in */action.yml) scan_action_default "$f";; esac; } )
done < <(find .github -type f \( -name '*.yml' -o -name '*.yaml' -o -name '*.sh' \) | sort)

[ "$found_any" = 1 ] || bad "found no xlings pins at all in .github/ — the scanner's patterns have gone stale, which would make this guard silently vacuous"

# ── 2. mcpp's own version ─────────────────────────────────────────────────

v_toml=$(awk -F '"' '/^version[[:space:]]*=/{print $2; exit}' mcpp.toml)
v_src=$(grep -oE 'MCPP_VERSION[[:space:]]*=[[:space:]]*"[^"]+"' src/toolchain/fingerprint.cppm \
        | grep -oE '"[^"]+"' | tr -d '"' | head -1)
v_xl=$(grep -oE '"mcpp"[[:space:]]*:[[:space:]]*"[^"]+"' .xlings.json \
       | grep -oE '"[^"]+"$' | tr -d '"' | head -1)
v_pin=$(grep -oE "MCPP_PIN:[[:space:]]*'[^']+'" .github/workflows/ci-fresh-install.yml \
        | grep -oE "'[^']+'" | tr -d "'" | head -1)

note "mcpp version: mcpp.toml=$v_toml fingerprint=$v_src .xlings.json=$v_xl MCPP_PIN=$v_pin"

for pair in "mcpp.toml:$v_toml" \
            "src/toolchain/fingerprint.cppm:$v_src" \
            ".xlings.json:$v_xl" \
            ".github/workflows/ci-fresh-install.yml (MCPP_PIN):$v_pin"; do
  where="${pair%:*}"; val="${pair##*:}"
  [ -n "$val" ] || { bad "$where — could not read the mcpp version"; continue; }
  [ "$val" = "$v_toml" ] || bad "$where has '$val' but mcpp.toml has '$v_toml'"
done

if [ "$fail" = 0 ]; then
  echo "OK: xlings pins all at $XLINGS_EXPECTED; mcpp version $v_toml consistent in 4 places" >&2
fi
exit "$fail"
