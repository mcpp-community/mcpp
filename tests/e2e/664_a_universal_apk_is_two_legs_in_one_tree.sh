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
"$MCPP" pack myapp --target aarch64-linux-android --pkg-format dir > pack1.log 2>&1 \
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
        --pkg-format dir > pack2.log 2>&1 \
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

# ── 3. other rows: one package per leg (design 2026-10-29 §4.2) ──────────
#
# `x86_64-linux-gnu` is not Android: `application_form` answers `Executable`
# there, so the rows are not one APK. A target named twice is one leg; an
# Android row and an executable row are a package each, named by its triple,
# and `-o` naming one file is refused.
"$MCPP" pack myapp --target x86_64-linux-gnu --target x86_64-linux-gnu --pkg-format dir > pack3.log 2>&1 \
    || fail "a target named twice is one leg" pack3.log
[ -d target/dist/myapp-0.1.0-x86_64-linux-gnu ] || fail "the one leg's package is not named by its triple" pack3.log
echo "a target named twice is one leg OK"

"$MCPP" pack myapp --target aarch64-linux-android --target x86_64-linux-gnu > pack4.log 2>&1 \
    || fail "an Android row and an executable row are a package each" pack4.log
ls target/dist/myapp-0.1.0-x86_64-linux-gnu*.tar.gz > /dev/null 2>&1 \
    || fail "the executable row's package is not named by its triple" pack4.log
if "$MCPP" pack myapp --target aarch64-linux-android --target x86_64-linux-gnu -o one.tar.gz > pack5.log 2>&1; then
    fail "-o naming one file was accepted for two packages" pack5.log
fi
grep -q "use: -o <directory>/" pack5.log || fail "the refusal names no directory" pack5.log
echo "an Android row and an executable row are a package each OK"

echo "664: the universal APK is the library route applied to an app OK"
