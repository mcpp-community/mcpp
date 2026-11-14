#!/usr/bin/env bash
# tests/canary/run.sh — the canaries over the matrix (design 2026-11-12 §5.7).
#
#   tests/canary/run.sh [--host <host>] [--only <canary>] [--report <file.tsv>]
#
# Every row of tests/canary/matrix.tsv whose host is this machine's is run: the
# step in the canary's directory, against the identity, with `$MCPP` (default
# `mcpp`). A row states what the step is expected to do:
#
#   ok      it succeeds; a program run prints no FAIL line and returns 0
#   fails   it does not, today, for the reason the row's note names
#   n/a     the step does not apply to the identity (the row is not run)
#
# The report (TSV: canary identity host step expect got seconds) is written to
# --report, and a Markdown table to $GITHUB_STEP_SUMMARY when set. The status is
# the number of rows whose result differs from what they state, either way: a
# row that now succeeds where it stated `fails` is a change of behaviour, and the
# table is corrected with it.
set -u
here=$(cd "$(dirname "$0")" && pwd)
MCPP=${MCPP:-mcpp}
host=""; only=""; report=""
while [ $# -gt 0 ]; do
  case "$1" in
    --host) host=$2; shift 2 ;;
    --only) only=$2; shift 2 ;;
    --report) report=$2; shift 2 ;;
    *) echo "usage: $0 [--host <host>] [--only <canary>] [--report <file>]" >&2; exit 2 ;;
  esac
done
if [ -z "$host" ]; then
  case "$(uname -s)" in
    Linux) os=linux ;; Darwin) os=macos ;; MINGW*|MSYS*|CYGWIN*) os=windows ;; FreeBSD) os=freebsd ;; *) os=unknown ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;; aarch64|arm64) arch=aarch64 ;; *) arch=$(uname -m) ;;
  esac
  host=$os-$arch
fi
report=${report:-$PWD/canary-report-$host.tsv}
logs=$(mktemp -d)
printf 'canary\tidentity\thost\tstep\texpect\tgot\tseconds\n' > "$report"
differ=0; ran=0
while IFS=$'\t' read -r canary identity row_host step expect note; do
  case "$canary" in ''|'#'*) continue ;; esac
  [ "$row_host" = "$host" ] || continue
  [ -z "$only" ] || [ "$only" = "$canary" ] || continue
  [ "$expect" = "n/a" ] && continue
  dir=$here/$canary
  # `${prog[@]+...}` below: bash 3.2 (macOS) refuses an empty array under `set -u`.
  prog=(); [ "$canary" = c ] && prog=(canary-a)
  case "$step" in
    build)  cmd=("$MCPP" build --target "$identity") ;;
    static) cmd=("$MCPP" build --static --target "$identity") ;;
    run)    cmd=("$MCPP" run ${prog[@]+"${prog[@]}"} --target "$identity" -- x last) ;;
    test)   cmd=("$MCPP" test --target "$identity") ;;
    pack)   cmd=("$MCPP" pack --target "$identity") ;;
    *) echo "matrix: unknown step '$step'" >&2; exit 2 ;;
  esac
  log=$logs/$canary-$identity-$step.log
  start=$(date +%s)
  (cd "$dir" && "${cmd[@]}") > "$log" 2>&1
  rc=$?
  seconds=$(( $(date +%s) - start ))
  got=ok
  if [ $rc -ne 0 ] || { [ "$step" = run ] && grep -q '^FAIL' "$log"; }; then got=fails; fi
  ran=$((ran + 1))
  mark=""
  if [ "$got" != "$expect" ]; then
    differ=$((differ + 1)); mark="  <- states $expect"
    # The whole log of a short run: its head holds the resolution and the failed
    # command, which a tail of linker errors pushes out.
    echo "::group::$canary $identity $step: $got (the row states $expect)"
    if [ "$(wc -l < "$log")" -le 300 ]; then cat "$log"; else head -150 "$log"; echo "[...]"; tail -100 "$log"; fi
    # A cell that fails where its row states ok is run again with --verbose:
    # the commands it ran (a link line's flags) say why.
    if [ "$got" = fails ]; then
      echo "--- again, with --verbose: the commands ---"
      (cd "$dir" && "${cmd[0]}" "${cmd[1]}" --verbose "${cmd[@]:2}") 2>&1 | grep -E '(clang|g\+\+|gcc|ld|lld)[^ ]* .* -o ' | tail -5 | cut -c1-4000
    fi
    echo "::endgroup::"
  fi
  printf '%-4s %-22s %-7s %-6s %3ss%s\n' "$canary" "$identity" "$step" "$got" "$seconds" "$mark"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$canary" "$identity" "$host" "$step" "$expect" "$got" "$seconds" >> "$report"
done < "$here/matrix.tsv"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### Canaries on $host: $ran cells, $differ differ from the matrix"
    echo
    echo "| canary | identity | step | states | got | s |"
    echo "|---|---|---|---|---|---|"
    tail -n +2 "$report" | awk -F'\t' '{ m = ($5 == $6) ? $6 : "**" $6 "**"; printf "| %s | %s | %s | %s | %s | %s |\n", $1, $2, $4, $5, m, $7 }'
  } >> "$GITHUB_STEP_SUMMARY"
fi
rm -rf "$logs"
echo "canaries on $host: $ran cells run, $differ differ from tests/canary/matrix.tsv"
exit $((differ > 0 ? 1 : 0))
