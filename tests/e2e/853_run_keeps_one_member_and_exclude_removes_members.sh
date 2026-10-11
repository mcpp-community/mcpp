#!/usr/bin/env bash
# requires: unix-shell
# 853 -- `mcpp run` keeps one member, and `--exclude` removes members from a
# whole-workspace selection.
#
# `-p` is repeatable on `build`, `test` and `mcpp emit build-database`, and
# one selection function serves all of them (member selection design
# 2026-09-30, S1 to S3). `run` executes one program, so a second `-p` there is
# refused, naming both, and never read as "the last one". `--exclude <name>`
# removes members from the forms that select every member: `--workspace`, or a
# virtual root without `-p`.
#
# Criteria:
#   D. `mcpp run -p a -p b` is refused, naming both, before anything is
#      planned; `mcpp run -p a` runs `a`.
#   E. `mcpp build --workspace --exclude c` builds `a` and `b` and leaves `c`
#      untouched; so does `--exclude c` alone at a virtual root. `--exclude
#      nosuch`, `-p a --exclude b`, and an `--exclude` that leaves no member
#      are refused, each before planning; so is `--exclude` where no
#      whole-workspace form applies, and `--workspace` together with `-p`.
#   F. `mcpp test --workspace --exclude c` tests `a` and `b`, and `mcpp emit
#      build-database` describes the members a selection names and no others.
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
    mkdir -p $m/src $m/tests
    printf '[package]\nname    = "%s"\nversion = "0.1.0"\n\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' $m $m > $m/mcpp.toml
    printf '#include <cstdio>\nint main() { std::puts("hello from %s"); return 0; }\n' $m > $m/src/main.cpp
    printf 'int main() { return 0; }\n' > $m/tests/smoke.cpp
done

build_dir() { find target -name build.ninja -not -path '*/.build-mcpp/*' -exec dirname {} \; | head -1; }
refused() {   # refused <label> <log> <command...>: non-zero, no planning, nothing built
    local label=$1 log=$2; shift 2
    local rc=0
    "$@" > "$log" 2>&1 || rc=$?
    [ "$rc" -ne 0 ] || fail "$label: the command was accepted" "$log"
    ! grep -q "Resolving toolchain" "$log" || fail "$label: the command planned before it refused" "$log"
    [ ! -d target ] || fail "$label: the refused command left a build directory" "$log"
}

# ── D ──────────────────────────────────────────────────────────────────────
refused D d1.log "$MCPP" run -p a -p b
grep -q "'a'" d1.log && grep -q "'b'" d1.log || fail "D: the refusal does not name both members" d1.log
"$MCPP" run -p a > d2.log 2>&1 || fail "D: -p a did not run" d2.log
grep -q "hello from a" d2.log || fail "D: -p a did not run member a" d2.log
! grep -q "hello from b" d2.log || fail "D: -p a ran member b" d2.log
rm -rf target
echo "ok: D, run refuses a second -p by name and runs one member"

# ── E ──────────────────────────────────────────────────────────────────────
refused E1 e1.log "$MCPP" build --workspace --exclude nosuch
grep -q "nosuch" e1.log || fail "E: the refusal does not name the member it could not find" e1.log
refused E2 e2.log "$MCPP" build -p a --exclude b
grep -q -- "--exclude" e2.log || fail "E: the refusal does not name --exclude" e2.log
refused E3 e3.log "$MCPP" build --workspace --exclude a --exclude b --exclude c
grep -q "every member" e3.log || fail "E: the refusal does not say that nothing is left" e3.log
(cd a && refused E4 ../e4.log "$MCPP" build --exclude b) || exit 1
grep -q -- "--workspace" e4.log || fail "E: the refusal does not name --workspace" e4.log
# `--workspace` and `-p` state two selections; neither is taken over the other.
refused E6 e6.log "$MCPP" build --workspace -p a
grep -q -- "--workspace" e6.log && grep -q -- "-p" e6.log \
    || fail "E: the refusal of --workspace with -p does not name both" e6.log

"$MCPP" build --workspace --exclude c > e5.log 2>&1 || fail "E: --workspace --exclude c did not build" e5.log
dir=$(build_dir)
[ -e "$dir/obj/a" ] && [ -e "$dir/obj/b" ] || fail "E: a and b were not built" e5.log
[ ! -e "$dir/obj/c" ] || fail "E: c was built although it was excluded" e5.log
rm -rf target
# A virtual root selects every member without --workspace, so --exclude applies
# to that selection too, however many it is spelled with.
"$MCPP" build --exclude c --exclude ./b > e6.log 2>&1 || fail "E: --exclude at a virtual root did not build" e6.log
dir=$(build_dir)
[ -e "$dir/obj/a" ] || fail "E: a was not built" e6.log
[ ! -e "$dir/obj/b" ] && [ ! -e "$dir/obj/c" ] || fail "E: an excluded member was built" e6.log
rm -rf target
echo "ok: E, --exclude removes members from an all form and is refused elsewhere, before planning"

# ── F ──────────────────────────────────────────────────────────────────────
"$MCPP" test --workspace --exclude c > f1.log 2>&1 || fail "F: test --workspace --exclude c failed" f1.log
grep -q "member 'a' (1/2) ok" f1.log && grep -q "member 'b' (2/2) ok" f1.log \
    || fail "F: test did not report exactly a and b" f1.log
! grep -q "member 'c'" f1.log || fail "F: test touched the excluded member" f1.log
grep -q "workspace result ok. 2 member(s)" f1.log || fail "F: the workspace result does not count two members" f1.log
rm -rf target

"$MCPP" emit build-database --format json -p a -p b > f2.json 2> f2.err || fail "F: emit -p a -p b failed" f2.err
grep -q '"family-name": "a"' f2.json && grep -q '"family-name": "b"' f2.json \
    || fail "F: the database does not describe a and b" f2.json
! grep -q '"family-name": "c"' f2.json || fail "F: the database describes c" f2.json
"$MCPP" emit build-database --format json --workspace --exclude a > f3.json 2> f3.err || fail "F: emit --exclude failed" f3.err
grep -q '"family-name": "b"' f3.json && grep -q '"family-name": "c"' f3.json || fail "F: the database does not describe b and c" f3.json
! grep -q '"family-name": "a"' f3.json || fail "F: the database describes the excluded member" f3.json
rc=0
"$MCPP" emit build-database --format json -p a --exclude b > f4.json 2> f4.err || rc=$?
[ "$rc" -ne 0 ] || fail "F: emit accepted -p with --exclude" f4.json
grep -q -- "--exclude" f4.json || fail "F: emit's refusal does not name --exclude" f4.json
echo "ok: F, test and emit build-database read the same selection"

echo "PASS: 853_run_keeps_one_member_and_exclude_removes_members"
