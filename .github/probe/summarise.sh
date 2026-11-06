#!/usr/bin/env bash
# Prints what one repetition observed; never asserts.
rep=$1; t=${RUNNER_TEMP:-/tmp}; p="$t/okprobe"
{
  echo "### rep $rep"
  echo '```'
  cat "$t/cross.rc" 2>/dev/null || echo "rc=<none>"
  grep -E "^mcpp: |^error: |^failed: |PROBE885" "$t/cross.log" 2>/dev/null | head -40
  echo '```'
  if ls "$p"/*.txt >/dev/null 2>&1; then
    echo "probe files: $(ls "$p" | wc -l), lines: $(cat "$p"/*.txt | wc -l)"
    echo '```'
    # Normalise names so the outcome classes count together.
    cat "$p"/*.txt | sed -E 's/slot=[^ ]+/slot=S/; s/spelled=[^ ]+/spelled=P/; s/rel_len=[0-9]+/rel_len=N/' \
      | sort | uniq -c | sort -rn | head -30
    echo '```'
    echo "non-ok OpenKal observations (first 20, full spelling):"
    echo '```'
    grep -h "^record" "$p"/*.txt | grep -vE "read=ok parent_stamp=ok root_identity=ok" | head -20
    echo '```'
  fi
} | tee -a "$GITHUB_STEP_SUMMARY"
