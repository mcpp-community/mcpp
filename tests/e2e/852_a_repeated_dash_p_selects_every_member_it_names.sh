#!/usr/bin/env bash
# requires: unix-shell
# 852 -- a repeated `-p` selects every member it names, as a set, and a `-p`
# that names no member is refused before anything is planned.
#
# `-p` was declared with `takes_value()` and without `multiple()`, so a
# repeated `-p` kept the last value and dropped the others without a word
# (mcpp#750): `mcpp build -p a -p b` built `b` alone and exited 0. The
# selection is now one function for every command (member selection design
# 2026-09-30, S1 and S2); the members it returns are a set in `[workspace]
# members` order.
#
# Criteria:
#   A. `mcpp build -p a -p b` in a workspace of `a`, `b` and `c` builds `a` and
#      `b`. `c`'s object directory stays absent.
#   B. `mcpp build -p a -p b` followed by `mcpp build -p b -p a` adds no
#      compile edge to `.ninja_log`: the order `-p` was written in is not part
#      of the selection, and a member named twice is one member.
#   C. `mcpp build -p nosuch` exits non-zero before planning. Its message names
#      `nosuch` and lists the members; a `-p` that is valid beside it does not
#      rescue the command.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"

cat > mcpp.toml <<'EOF'
[workspace]
members = ["a", "b", "c"]
EOF
for m in a b c; do
    mkdir -p $m/src
    printf '[package]\nname    = "%s"\nversion = "0.1.0"\n\n[targets.%s]\nkind = "lib"\n' $m $m > $m/mcpp.toml
    printf 'export module sel852_%s;\nexport int %s_value() { return 1; }\n' $m $m > $m/src/$m.cppm
done

# The build directory of a command: the one directory under target/ holding a
# build.ninja.
build_dir() { find target -name build.ninja -not -path '*/.build-mcpp/*' -exec dirname {} \; | head -1; }
# How many times ninja built the object of member $2 in build directory $1.
object_edges() { awk -F'\t' -v m="$2" '$4 ~ ("(^|/)obj/" m "/src/" m "\\.m\\.o$")' "$1/.ninja_log" | wc -l | tr -d ' '; }

# ── C ──────────────────────────────────────────────────────────────────────
# First, so that nothing has been planned, or built, that could be mistaken for
# what the refusal left behind.
rc=0
"$MCPP" build -p a -p nosuch > c.log 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "C: a -p that names no member was accepted" c.log
grep -q "nosuch" c.log || fail "C: the refusal does not name the member it could not find" c.log
for m in a b c; do
    grep -qE "'$m'" c.log || fail "C: the refusal does not list member $m" c.log
done
! grep -q "Resolving toolchain" c.log || fail "C: the command planned before it refused" c.log
[ ! -d target ] || fail "C: the refused command left a build directory" c.log
echo "ok: C, an unknown -p is refused by name, with the members listed, before planning"

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" build -p a -p b > a.log 2>&1 || fail "A: -p a -p b did not build" a.log
dir=$(build_dir)
[ -n "$dir" ] || fail "A: no build directory" a.log
[ "$(object_edges "$dir" a)" = 1 ] || fail "A: a was not built" "$dir/.ninja_log"
[ "$(object_edges "$dir" b)" = 1 ] || fail "A: b was not built (the repeated -p kept one value)" "$dir/.ninja_log"
[ ! -e "$dir/obj/c" ] || fail "A: c was built although no -p named it" a.log
grep -q "Workspace building 2 members: a, b" a.log || fail "A: the plan does not state the two members" a.log
echo "ok: A, -p a -p b builds a and b, and c is untouched"

# ── B ──────────────────────────────────────────────────────────────────────
"$MCPP" build -p b -p a > b.log 2>&1 || fail "B: -p b -p a did not build" b.log
[ "$(object_edges "$dir" a)" = 1 ] && [ "$(object_edges "$dir" b)" = 1 ] \
    || fail "B: the reverse order compiled an edge again" "$dir/.ninja_log"
"$MCPP" build -p a -p ./b -p b -p a > b2.log 2>&1 || fail "B: a member named twice did not build" b2.log
[ "$(object_edges "$dir" a)" = 1 ] && [ "$(object_edges "$dir" b)" = 1 ] \
    || fail "B: naming a member twice compiled an edge again" "$dir/.ninja_log"
[ "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | wc -l | tr -d ' ')" = 1 ] || fail "B: the order made a second build directory" b.log
echo "ok: B, the order of -p, and a member named twice, do not change the selection"

echo "PASS: 852_a_repeated_dash_p_selects_every_member_it_names"
