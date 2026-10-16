#!/usr/bin/env bash
# requires: unix-shell fresh-sandbox
# 915 -- `--system` maintains the machine's system home; users read it as an
# upstream (docs/25 §4).
#
# The system configuration is the test's own ($MCPP_SYSTEM_CONFIG), naming a
# system home under the test's directory. Criteria:
#   A. `--system` with a command that is not a maintenance command is refused,
#      naming why, and writes nothing.
#   B. `--system self init` makes the system home: marked mode = "system",
#      every file readable by every user, whatever MCPP_HOME the caller has.
#   C. A system home this user cannot write is refused, naming sudo.
#   D. A user whose home names nothing reads the system configuration: its
#      upstream is the system home, and doctor says so.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

SYS="$TMP/system-home"
export MCPP_SYSTEM_CONFIG="$TMP/system.toml"
printf '[system]\nhome = "%s"\n\n[xlings]\nupstream = ["%s"]\n' "$(host_path "$SYS")" "$(host_path "$SYS")" > "$MCPP_SYSTEM_CONFIG"
# The machine's toolchain, as every isolated home has it.
export MCPP_HOME="$SYS"
source "$(dirname "$0")/_inherit_toolchain.sh"
export MCPP_HOME="$TMP/not-this-one"
cd "$TMP"

# ── A ─────────────────────────────────────────────────────────────────────────
if "$MCPP" --system build > a.log 2>&1; then fail "A: --system build was not refused" a.log; fi
grep -q "maintains the machine's system home" a.log || fail "A: the refusal does not say why" a.log
[ ! -e "$SYS/.mcpp-home" ] || fail "A: a refused --system wrote the system home"
echo "ok: A. --system takes maintenance commands only"

# ── B ─────────────────────────────────────────────────────────────────────────
"$MCPP" --system self init > b.log 2>&1 || fail "B: --system self init failed" b.log
grep -q '^mode = "system"' "$SYS/.mcpp-home" || fail "B: the system home is not marked" "$SYS/.mcpp-home"
[ ! -e "$TMP/not-this-one/registry" ] || fail "B: --system wrote the caller's MCPP_HOME"
unreadable=$(find "$SYS" -newer "$MCPP_SYSTEM_CONFIG" -type f ! -perm -o=r -print | head -5)
[ -z "$unreadable" ] || fail "B: files of the system home other users cannot read: $unreadable"
echo "ok: B. the system home is made, marked and readable by everyone"

# ── C ─────────────────────────────────────────────────────────────────────────
if [ "$(id -u)" != 0 ]; then
    chmod a-w "$SYS"
    if "$MCPP" --system index update > c.log 2>&1; then fail "C: an unwritable system home was written" c.log; fi
    grep -q "sudo" c.log || fail "C: the refusal does not name sudo" c.log
    chmod u+w "$SYS"
    echo "ok: C. a system home this user cannot write is refused"
fi

# ── D ─────────────────────────────────────────────────────────────────────────
export MCPP_HOME="$TMP/user"
mkdir -p "$MCPP_HOME"
"$MCPP" self doctor > d.log 2>&1 || true
grep -q "upstream $(host_path "$SYS")" d.log || fail "D: the user's home does not read the system configuration" d.log
echo "ok: D. a user reads the system home as an upstream"

echo "PASS: 915_the_system_home_is_maintained_and_read"
