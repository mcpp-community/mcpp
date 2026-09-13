#!/usr/bin/env bash
# requires: elf gcc android-ndk
# 664_a_universal_apk_is_two_legs_in_one_tree.sh -- #630 A9. The route is
# chosen by the artifact's FORM, not by the target's kind: a `kind = "app"`
# target whose link form is a shared object on every requested row (every
# Android row) accepts several `--target` triples, the way a library target
# already does (`build_and_pack_library`). Each leg is staged as
# `lib/<abi>/lib<name>.so` into ONE tree; the declared deploy files are
# staged once; one `mcpp pack` invocation, one staged tree.
#
# `--format dir` is the built-in format that hands the staged tree over
# without compressing it and without needing a `mcpp:plugins` member — see
# `pack::run`'s `Format::Tar` check in `run_shared_program`, which is the
# only place staging and format interact for a shared program.
set -e

t=$(mktemp -d); trap 'rm -rf "$t"' EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$t"
mkdir -p src
cat > mcpp.toml <<'TOML'
[package]
name    = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "app"
main = "src/main.cpp"

[[runtime.deploy]]
from = "res.txt"
to   = "myres"
TOML
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP
printf "resource-1\n" > res.txt

# ── 1. one triple: the staged tree is unchanged (flat lib/<name>.so) ──────
#
# This is the byte-identical direction (§ record item A9, "Do" #2): the
# multi-leg branch in `cmd_pack` is only reached for `triples.size() > 1`, so
# a single `--target` never enters it, and `Plan::extraSharedLegs` stays
# empty. Also asserted by 652b for the tarball route; repeated here, against
# `--format dir`, because THIS is the format the several-triple case below
# also uses, and the two must be compared under the same format.
"$MCPP" pack myapp --target aarch64-linux-android --format dir > pack1.log 2>&1 \
    || fail "single-target Android pack failed" pack1.log
staged1=$(ls -d target/dist/myapp-0.1.0-*/ 2>/dev/null | head -1)
[ -n "$staged1" ] || fail "no staged tree for the single-target pack" pack1.log
[ -f "${staged1}lib/libmyapp.so" ] \
    || fail "single-target pack: lib/libmyapp.so (flat) is missing" pack1.log
[ ! -d "${staged1}lib/arm64-v8a" ] \
    || fail "single-target pack introduced lib/arm64-v8a/ -- this must not change" pack1.log
[ -f "${staged1}bin/myres/res.txt" ] \
    || fail "single-target pack: the deploy'd file is missing" pack1.log
echo "one --target stages flat lib/libmyapp.so, unchanged OK"

# ── 2. two Android triples: one tree, two lib/<abi>/ legs ─────────────────
rm -rf target/dist
"$MCPP" pack myapp --target aarch64-linux-android --target x86_64-linux-android \
        --format dir > pack2.log 2>&1 \
    || fail "two-target Android pack failed" pack2.log
staged2=$(ls -d target/dist/myapp-0.1.0-*/ 2>/dev/null | head -1)
[ -n "$staged2" ] || fail "no staged tree for the two-target pack" pack2.log

[ -f "${staged2}lib/arm64-v8a/libmyapp.so" ] \
    || fail "lib/arm64-v8a/libmyapp.so is missing" pack2.log
file "${staged2}lib/arm64-v8a/libmyapp.so" | grep -qi "ARM aarch64" \
    || fail "lib/arm64-v8a/libmyapp.so is not an aarch64 object" pack2.log

[ -f "${staged2}lib/x86_64/libmyapp.so" ] \
    || fail "lib/x86_64/libmyapp.so is missing" pack2.log
file "${staged2}lib/x86_64/libmyapp.so" | grep -qi "x86-64" \
    || fail "lib/x86_64/libmyapp.so is not an x86_64 object" pack2.log

[ ! -f "${staged2}lib/libmyapp.so" ] \
    || fail "the flat lib/libmyapp.so also exists -- the multi-leg tree must not carry it" pack2.log
echo "two --target flags stage lib/arm64-v8a/ and lib/x86_64/ in one tree OK"

# The declared deploy file travels ONCE, not once per leg.
depcount=$(find "${staged2}bin" -name res.txt | wc -l)
[ -f "${staged2}bin/myres/res.txt" ] || fail "the deploy'd file is missing from the multi-leg tree" pack2.log
[ "$depcount" -eq 1 ] \
    || fail "the deploy'd file was staged $depcount times, not once" pack2.log
echo "the deploy'd file is staged once across both legs OK"

# ── 3. negative direction: an executable-form row refuses a second triple ─
#
# `x86_64-linux-gnu` is not Android: `application_form` answers `Executable`
# there, so `accepts_several_targets` must refuse this request exactly as it
# refused before #630 A9 existed.
out=$("$MCPP" pack myapp --target x86_64-linux-gnu --target x86_64-linux-gnu 2>&1) \
    && fail "packing an app for two executable-form triples must be refused" <(echo "$out")
expected="--target may be given once when packing a program: an application bundle wraps one executable, and one executable has one target."
grep -qF -- "$expected" <<<"$out" \
    || fail "the refusal message does not match today's wording" <(echo "$out")
echo "two executable-form triples are refused with today's message OK"

# Mixed direction: one Android (shared-object) row and one host (executable)
# row is not "every row is a shared object" either.
out=$("$MCPP" pack myapp --target aarch64-linux-android --target x86_64-linux-gnu 2>&1) \
    && fail "mixing a shared-object row with an executable row must be refused" <(echo "$out")
grep -qF -- "$expected" <<<"$out" \
    || fail "the mixed-row refusal message does not match today's wording" <(echo "$out")
echo "a shared-object row mixed with an executable row is refused OK"

echo "664: the universal APK is the library route applied to an app OK"
