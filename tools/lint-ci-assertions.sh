#!/usr/bin/env bash
# lint-ci-assertions — where an assertion is written, checked mechanically.
#
# THIS EXISTS BECAUSE THE SAME MISTAKE WAS MADE THREE TIMES, AND EACH TIME
# THE TEST WAS GREEN.
#
#   1. (openarch 0.4.0) A template check sat in a job that installs no
#      emulator. It could therefore only ever assert `mcpp build`, and a
#      generated project that compiled for three machines and ran on none of
#      them passed it.
#
#   2. (openarch 0.5.0) Moved to a job that HAS emulators, it was gated to ONE
#      matrix row with `if: matrix.arch == 'riscv64'`. Each row installs only
#      its own machine's emulator, so the claim it could support was "the
#      template runs on riscv64" while the README it guarded claimed all three.
#      It did not: the template's own build program still had the third
#      machine's early return in it.
#
#   3. (qemu-x86) A payload-closure check was written as
#      `ldd "$B" 2>/dev/null | grep -v <payload>`. It printed nothing and read
#      exactly like "nothing escapes" — the `ldd` on that PATH was a shell
#      script that failed to parse, and it failed on STDERR.
#
# One shape: an assertion must live where the phenomenon it claims can be
# observed, and a matrix ROW is a separate observation environment from its job.
#
# WHAT THIS CANNOT DO, STATED FIRST SO IT IS NOT DISCOVERED LATER.
#
# Case 1 above is NOT detectable here, and the attempt to detect it is what
# taught the limit. Its defect was an assertion that was ABSENT — the step
# asserted `mcpp build` and claimed only that. Nothing in the file is wrong; the
# file is merely weaker than the claim the repository makes elsewhere. **A
# linter checks what is written, not what is missing.** R4 below catches the
# same SHAPE going forward (a run assertion placed where nothing can run), which
# is the most this mechanism can offer.
#
# WARNINGS, NOT FAILURES. These rules have real false positives — a control
# step deliberately pins one target; some greps legitimately expect empty output
# from a tool that cannot fail. The value here is being READ. A hard gate would
# be routed around with a suppression within a month.
#
# Suppress one line with a trailing `ci-lint: allow-<rule>` comment plus a
# reason.
#
# Usage:  tools/lint-ci-assertions.sh [file-or-dir ...]   (default .github/workflows)
set -uo pipefail

TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(".github/workflows")

FILES=()
for t in "${TARGETS[@]}"; do
  if [ -d "$t" ]; then
    while IFS= read -r f; do FILES+=("$f"); done < <(find "$t" -type f \( -name '*.yml' -o -name '*.yaml' \) | sort)
  elif [ -f "$t" ]; then
    FILES+=("$t")
  fi
done

findings=0
note() { printf '%s:%s: [%s] %s\n' "$1" "$2" "$3" "$4"; findings=$((findings + 1)); }
allowed() { case "$1" in *"ci-lint: allow-$2"*) return 0 ;; esac; return 1; }

for f in "${FILES[@]}"; do
  [ -s "$f" ] || continue

  # ── R1: a step gated to ONE value of a matrix axis ────────────────────────
  #
  # `if: matrix.arch == 'riscv64'` says in one line that this step observes one
  # row. That is sometimes right — a host-independent claim needs asserting
  # once, not N times — and it is exactly what silently narrows a claim that
  # has since grown. The rule asks for the reason to be written down.
  while IFS=: read -r ln text; do
    [ -z "${ln:-}" ] && continue
    allowed "$text" r1 && continue
    axis=$(printf '%s' "$text" | grep -oE "matrix\.[A-Za-z_][A-Za-z0-9_]*" | head -1)
    note "$f" "$ln" R1 \
      "step gated to one value of ${axis:-a matrix axis} — it can assert only that row's environment. If the claim is per-row, drop the gate; if it is host-independent, say so with 'ci-lint: allow-r1: <reason>'"
  done < <(grep -nE "^\s+if:\s*matrix\.[A-Za-z_][A-Za-z0-9_]*\s*==" "$f" 2>/dev/null)

  # ── R3: a check that reads emptiness as success ───────────────────────────
  #
  # `cmd 2>/dev/null | grep -v <expected>` turns "the tool did not run" into
  # "nothing was wrong". Discarding stderr is what makes the two
  # indistinguishable.
  #
  # NARROWED TO VERDICTS-BY-ABSENCE, and the narrowing came from running it.
  # `cmd 2>/dev/null | grep -q .` looks identical to a machine and is the
  # OPPOSITE thing — it asserts the output is non-empty, which is what this rule
  # asks for. Only `-v` (everything except the expected), `-c` (a count that is
  # zero when the tool dies) and a negated pipeline judge by absence.
  while IFS=: read -r ln text; do
    [ -z "${ln:-}" ] && continue
    allowed "$text" r3 && continue
    note "$f" "$ln" R3 \
      "stderr is discarded and the verdict is absence of output — a tool that fails to run looks identical to a clean result. Assert the inspected set is non-empty, or keep stderr"
  done < <(grep -nE '2>\s*/dev/null[^|]*\|\s*(!\s*)?grep\s+(-[A-Za-z]*[vc][A-Za-z]*)\b' "$f" 2>/dev/null)

  # ── R4: an execution assertion in a job that installs nothing to execute ──
  #
  # The forward-looking half of case 1. Per job: if any step runs a FREESTANDING
  # artifact (`mcpp run --target <...>-none-<...>`) or an emulator binary
  # directly, some step in the SAME job must install one.
  #
  # Restricted to freestanding targets, and the restriction came from running
  # it: a plain `mcpp run` on a hosted target needs no emulator and fired on six
  # jobs that were entirely correct. A rule with that hit rate is not read.
  awk -v file="$f" '
    function flush_job(  ) {
      if (job != "" && runs > 0 && installs == 0)
        printf "%s:%d: [R4] job \047%s\047 asserts execution (mcpp run / qemu-system-*) but installs no emulator — the assertion cannot observe what it claims\n", file, runline, job
    }
    /^  [A-Za-z_][A-Za-z0-9_-]*:[[:space:]]*$/ { flush_job(); job=$1; sub(/:$/,"",job); runs=0; installs=0; runline=0; next }
    /ci-lint: allow-r4/ { installs++ ; next }
    /mcpp run[^\n]*--target[^\n]*-none-|qemu-system-/ { if (!/install/) { runs++; if (runline==0) runline=NR } }
    /xlings install .*qemu|apt-get install .*qemu|brew install .*qemu|xim:qemu/ { installs++ }
    END { flush_job() }
  ' "$f" | while IFS= read -r line; do printf '%s\n' "$line"; findings=$((findings+1)); done
  # The awk findings are counted by re-scanning, because the pipeline above runs
  # in a subshell and its increments do not survive.
  r4=$(awk -v file="$f" '
    function flush_job(  ) { if (job != "" && runs > 0 && installs == 0) n++ }
    /^  [A-Za-z_][A-Za-z0-9_-]*:[[:space:]]*$/ { flush_job(); job=$1; runs=0; installs=0; next }
    /ci-lint: allow-r4/ { installs++ ; next }
    /mcpp run[^\n]*--target[^\n]*-none-|qemu-system-/ { if (!/install/) runs++ }
    /xlings install .*qemu|apt-get install .*qemu|brew install .*qemu|xim:qemu/ { installs++ }
    END { flush_job(); print n+0 }
  ' "$f")
  findings=$((findings + r4))
done

# ── R2: declared file pairs that must stay byte-identical ────────────────────
#
# One piece of logic in two files is what shipped a template whose build program
# disagreed with its own README. The fix is not "be more careful" — that had
# already been tried once — it is to assert sameness.
#
# Pairs are declared in .ci-identical-files, one `a<TAB>b` per line. An optional
# third field is a marker: only the text from that marker onward is compared,
# for the common case of two files that share a body and differ in a header.
PAIRS=".ci-identical-files"
if [ -f "$PAIRS" ]; then
  while IFS=$'\t' read -r a b marker; do
    case "${a:-}" in ''|'#'*) continue ;; esac
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
      note "$PAIRS" 0 R2 "declared pair is missing a side: '$a' / '$b'"
      continue
    fi
    if [ -n "${marker:-}" ]; then
      ta=$(mktemp); tb=$(mktemp)
      sed -n "/$(printf '%s' "$marker" | sed 's/[]\/$*.^[]/\\&/g')/,\$p" "$a" > "$ta"
      sed -n "/$(printf '%s' "$marker" | sed 's/[]\/$*.^[]/\\&/g')/,\$p" "$b" > "$tb"
      if [ ! -s "$ta" ] || [ ! -s "$tb" ]; then
        # An absent marker would make both sides empty and the comparison
        # would pass — the emptiness-as-success shape this file exists to name.
        note "$PAIRS" 0 R2 "marker '$marker' not found in '$a' or '$b'; the comparison would have passed on two empty texts"
      elif ! diff -q "$ta" "$tb" > /dev/null 2>&1; then
        note "$PAIRS" 0 R2 "'$a' and '$b' diverge after '$marker' and are declared identical"
      fi
      rm -f "$ta" "$tb"
    elif ! diff -q "$a" "$b" > /dev/null 2>&1; then
      note "$PAIRS" 0 R2 "'$a' and '$b' are declared identical and are not"
    fi
  done < "$PAIRS"
fi

if [ "$findings" -eq 0 ]; then
  echo "ci-assertions: no findings in ${#FILES[@]} workflow file(s)"
else
  echo "ci-assertions: $findings finding(s) — warnings, not failures; see tools/lint-ci-assertions.sh"
fi
exit 0
