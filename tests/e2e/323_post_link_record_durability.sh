#!/usr/bin/env bash
# requires: elf
# 323_post_link_record_durability.sh — the post-link verdicts survive an
# invocation, and survive the OTHER command (#529).
#
# Both post-link ELF passes were written with a read-back: an artifact whose
# stat did not move keeps the verdict already on file. It never fired across
# invocations, because the file it read was `resolution.json` and
# `prepare_build` rewrites that from an empty object at the start of every run.
# Measured before the fix: 1.36 s of a 1.94 s warm `mcpp test`.
#
# WHY A SINGLE-COMMAND TEST CANNOT SEE ANY OF THIS. Within one invocation
# the read-back already worked — the second ninja drive paid nothing. And
# repeating one command hides the second half: `mcpp build` and `mcpp test`
# share an output directory with DIFFERENT link-unit sets, and pruning the
# record against the current plan makes each delete the other's verdicts. So
# the assertions below are across processes AND across commands.
#
# The timing is deliberately not asserted — it is a machine property. What is
# asserted is the record: its CONTENT is what makes the memo correct, and a
# memo that is correct is what makes the build fast.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p src tests
printf '[package]\nname = "dur"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
printf 'int main(){ return 0; }\n' > src/main.cpp
for t in one two; do printf 'int main(){ return 0; }\n' > "tests/t_$t.cpp"; done

"$MCPP" build > b1.log 2>&1 || { cat b1.log; exit 1; }
OUT=$(dirname "$(find target -name build.ninja | head -1)")
[ -n "$OUT" ] || { echo "FAIL: no build.ninja"; exit 1; }

REC="$OUT/.mcpp-runtime-verdicts.json"
RES="$OUT/resolution.json"

paths_in() {  # $1 = file, $2 = record name
    sed -n "/\"$2\"/,/^ *\]/p" "$1" 2>/dev/null \
        | grep -oE 'bin/[A-Za-z_0-9]+' | sort -u | tr '\n' ' '
}

# ── the record must survive `prepare_build`, which is the whole defect ──────
[ -f "$REC" ] || { echo "FAIL: no durable record was written"; exit 1; }
grep -q 'post_link_key' "$REC" || {
    echo "FAIL: the record carries no invalidation key — a stale SubOS farm or"
    echo "      a flipped MCPP_ALLOW_HOST_LIBS would be answered from cache"
    exit 1
}

after_build=$(paths_in "$REC" loader_tags)
case "$after_build" in
    *bin/dur*) ;;
    *) echo "FAIL: the package binary is not in the record after a build"
       echo "      got: $after_build"; exit 1 ;;
esac

# ── `mcpp test` must ADD to it, not replace it ──────────────────────────────
#
# THE ASSERTION WITH A DENOMINATOR. `mcpp test`'s plan contains the test
# binaries and NOT `bin/dur`; a record pruned against the current plan would
# come back holding only the tests, and "the pass was skipped" would then be
# satisfied by "the pass had nothing to look at".
"$MCPP" test > t1.log 2>&1 || { cat t1.log; exit 1; }
after_test=$(paths_in "$REC" loader_tags)
for want in bin/dur bin/t_one bin/t_two; do
    case "$after_test" in
        *"$want"*) ;;
        *) echo "FAIL: after 'mcpp test' the record lost or never gained $want"
           echo "      got: $after_test"; exit 1 ;;
    esac
done

sp_after_test=$(paths_in "$REC" symbol_provision)
for want in bin/dur bin/t_one bin/t_two; do
    case "$sp_after_test" in
        *"$want"*) ;;
        *) echo "FAIL: symbol_provision record is missing $want"
           echo "      got: $sp_after_test"; exit 1 ;;
    esac
done

# ── and a following `mcpp build` must not delete the test verdicts ──────────
"$MCPP" build > b2.log 2>&1 || { cat b2.log; exit 1; }
after_build2=$(paths_in "$REC" loader_tags)
for want in bin/dur bin/t_one bin/t_two; do
    case "$after_build2" in
        *"$want"*) ;;
        *) echo "FAIL: 'mcpp build' pruned the test binaries out of the record."
           echo "      The two commands share one output directory; pruning"
           echo "      against the current plan makes each erase the other."
           echo "      got: $after_build2"; exit 1 ;;
    esac
done

# ── resolution.json keeps publishing both, on every build ───────────────────
#
# The sidecar is authoritative; `resolution.json` is the documented place to
# read the verdicts (docs/05, `mcpp why runtime`, e2e 214 and 307). Publishing
# only when the content changed left it empty on every warm build — the record
# correct and the place people look for it empty.
for name in loader_tags symbol_provision; do
    grep -q "\"$name\"" "$RES" || {
        echo "FAIL: resolution.json does not publish $name after a warm build"
        exit 1
    }
    published=$(paths_in "$RES" "$name")
    case "$published" in
        *bin/dur*) ;;
        *) echo "FAIL: resolution.json's $name is empty on a warm build"
           echo "      got: '$published'"; exit 1 ;;
    esac
done

# ── an artifact that leaves the DISK leaves the record ──────────────────────
#
# The pruning still has to happen, or the file grows forever. What changed is
# the predicate: gone from disk, not absent from this command's plan.
rm -f "$OUT/bin/t_two"
"$MCPP" build > b3.log 2>&1 || { cat b3.log; exit 1; }
final=$(paths_in "$REC" loader_tags)
case "$final" in
    *bin/t_two*) echo "FAIL: a deleted artifact stayed in the record"
                 echo "      got: $final"; exit 1 ;;
esac
case "$final" in
    *bin/dur*) ;;
    *) echo "FAIL: pruning removed more than the deleted artifact"
       echo "      got: $final"; exit 1 ;;
esac

echo "PASS: 323_post_link_record_durability"
