#!/usr/bin/env bash
# THE PROBE'S RECORDER (temporary; this branch is never merged). The e2e kit runs
# `MCPP_E2E_MCPP` in place of the mcpp under test; this runs that mcpp with the same
# arguments and records the invocation: when it began and ended, the CPU its process
# tree used (the children's `times`), its exit status, its directory and arguments.
# After it, every ninja log under the directory's target/ is copied, numbered by the
# invocation, so that the edges each build ran can be read per configuration.
set -u
# The setup step writes this file with the two paths in place of the placeholders, so
# that the recorder does not depend on what the test runner passes down.
real="${PROBE_REAL_MCPP:-@REAL@}"
dir="${PROBE_DIR:-@DIR@}"
# The phase of the probe (cold, warm), which the workflow writes before each pass.
phase="$(cat "$dir/phase" 2>/dev/null || true)"
mkdir -p "$dir/ninja"
n="$(date +%s%N)-$$"
# The package the command acts on, read before it runs (a case's directory is removed
# after the case).
pkg="$(sed -n 's/^name *= *"\(.*\)"/\1/p' mcpp.toml 2>/dev/null | head -n 1)"
start="$EPOCHREALTIME"
"$real" "$@"
rc=$?
end="$EPOCHREALTIME"
# `times` in this shell, not in a pipeline or a substitution: a subshell has waited
# for nothing, and its children's times are zero. The second line is the children's:
# the mcpp above and every process below it that was waited for.
t="$dir/.times-$n"
times > "$t"
secs() { awk -v v="$1" 'BEGIN { split(v, a, "m"); sub("s", "", a[2]); printf "%.3f", a[1] * 60 + a[2] }'; }
read -r cu cs < <(tail -n 1 "$t")
rm -f "$t"
args="$(printf '%s\037' "$@")"
python3 -I - "$dir/invocations.jsonl" "$n" "$start" "$end" "$(secs "$cu")" "$(secs "$cs")" "$rc" "$PWD" "$phase" "$args" "$pkg" <<'PYEOF'
import json, sys
out, n, start, end, user, system, rc, cwd, phase, args, pkg = sys.argv[1:12]
record = {"n": n, "start": float(start), "end": float(end), "wall": round(float(end) - float(start), 3),
          "user": float(user), "sys": float(system), "rc": int(rc), "cwd": cwd, "pass": phase,
          "argv": [a for a in args.split("\x1f") if a], "pkg": pkg}
with open(out, "a", encoding="utf-8") as f:
    f.write(json.dumps(record) + "\n")
PYEOF
if [ -d "$PWD/target" ]; then
  while IFS= read -r -d '' log; do
    rel="${log#"$PWD"/}"
    mkdir -p "$dir/ninja/$n"
    cp "$log" "$dir/ninja/$n/$(printf '%s' "$rel" | tr '/' '%')"
  done < <(find "$PWD/target" -name .ninja_log -print0 2>/dev/null)
fi
exit "$rc"
