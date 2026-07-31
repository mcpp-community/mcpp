#!/usr/bin/env bash
# requires: unix-shell
# 178_test_observability.sh — `mcpp test` is streamed, timed and bounded.
#
# Four properties, each of which used to be absent and each of which cost a real
# CI investigation:
#
#   1. stdout is line-buffered even into a pipe. It used to be block-buffered
#      with a libc-dependent block SIZE (musl 1 KB, Apple libc 64 KB on a pipe),
#      so the same ~13 KB of status output flushed 13 times on Linux and zero
#      times on macOS — and when the job timeout then killed the process, the
#      whole buffer went with it: a 45-minute step with no attributable output.
#   2. Per-test and per-member timings are printed, split into build vs run.
#      The old `finished in` started its clock AFTER the package build, so a
#      member that spent 87s linking reported 6.53s.
#   3. `--timeout` bounds a hung test RUN and the fan-out continues past it.
#   4. `--build-timeout` bounds a hung compile/link — the half `--timeout`
#      never covered, and the one that actually fires in practice.
#
# The `# requires:` line above must stay on line 2 — run_all.sh reads it with
# `sed -n '2p'`, so a requires buried in this block is silently inert (which is
# how an earlier revision of this file ran on Windows and failed there).
# unix-shell, not gcc: macOS is the platform this whole file exists for, and it
# has no GCC. Windows is excluded on purpose — the deadline runner has no
# kill-by-handle path there (mcpp.platform.process), so --timeout and
# --build-timeout are documented POSIX-only and cannot be asserted.
#
# See .agents/docs/2026-07-31-test-workspace-observability-analysis.md.
set -uo pipefail

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $*"; exit 1; }

# ── fixture: a fast member and a slow-to-RUN member ───────────────────────
mkdir -p fast/tests slow/tests
cat > mcpp.toml <<'EOF'
[workspace]
members = ["fast", "slow"]
EOF
for m in fast slow; do
  printf '[package]\nname = "%s"\nversion = "0.1.0"\n' "$m" > "$m/mcpp.toml"
done
cat > fast/tests/quick.cpp <<'EOF'
int main() { return 0; }
EOF
cat > slow/tests/hang.cpp <<'EOF'
#include <chrono>
#include <thread>
int main() { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
EOF

# ── 1. streaming: output arrives while the process is still running ───────
# The fan-out does `fast` first, then blocks 30s inside `slow`. Reading the
# fast member's result out of a pipe within a few seconds is only possible if
# mcpp flushed it; under block buffering nothing appears until exit.
# `<>` opens the fifo read-write, which never blocks and keeps both a reader and
# a writer alive for the whole test. Opening it read-only (or write-only) blocks
# until the other end appears, which is a deadlock waiting to happen — and a
# test written to make hangs debuggable must not be able to hang the suite.
mkfifo pipe
exec 3<>pipe
"$MCPP" test --workspace --timeout 25 >pipe 2>&1 &
mcpp_pid=$!
streamed=""
while IFS= read -r -t 25 line <&3; do
    case "$line" in
        *"quick ... ok"*) streamed=yes; break ;;
    esac
done
kill "$mcpp_pid" 2>/dev/null
wait "$mcpp_pid" 2>/dev/null
exec 3>&-
[ -n "$streamed" ] || fail "no output reached the pipe before the process ended (stdout not flushed)"
echo "  ok: stdout streams line-by-line into a pipe"

# ── 2. timings: per-test duration + build/run split + per-member progress ─
out=$("$MCPP" test -p fast 2>&1) || fail "test -p fast errored: $out"
echo "$out" | grep -qE "quick \.\.\. ok \([0-9]+\.[0-9]+s\)" \
    || { echo "$out"; fail "per-test line carries no duration"; }
echo "$out" | grep -qE "finished in [0-9]+\.[0-9]+s \(build [0-9]+\.[0-9]+s \+ run [0-9]+\.[0-9]+s\)" \
    || { echo "$out"; fail "summary carries no build/run split"; }
echo "  ok: per-test duration + build/run split"

# ── 3. --timeout bounds a hung RUN, and the fan-out continues ─────────────
out=$("$MCPP" test --workspace --timeout 2 2>&1)
rc=$?
echo "$out" | grep -q "hang ... FAIL (timeout after 2s)" \
    || { echo "$out"; fail "hung test was not killed by --timeout"; }
echo "$out" | grep -qE "member 'fast' \(1/2\) ok" \
    || { echo "$out"; fail "no per-member progress line"; }
echo "$out" | grep -q "failed members: slow" \
    || { echo "$out"; fail "workspace summary does not name the failed member"; }
[ "$rc" -ne 0 ] || fail "workspace test returned 0 despite a failed member"
echo "  ok: --timeout bounds a hung run; fan-out continues; member attributed"

# `--timeout 0` still means "no limit" — it just has to be asked for now.
"$MCPP" test -p fast --timeout 0 >/dev/null 2>&1 || fail "--timeout 0 rejected"

# ── 4. --build-timeout bounds a hung COMPILE ─────────────────────────────
# 20k distinct class templates at the default (optimized) profile: seconds to
# compile everywhere, so a 1s ceiling fires with a wide margin. The assertion is
# on the *reported reason*, which must name the build timeout rather than being
# folded into a generic compile failure.
mkdir -p heavy/tests
printf '[package]\nname = "heavy"\nversion = "0.1.0"\n' > heavy/mcpp.toml
{
  i=0
  while [ "$i" -lt 20000 ]; do
    printf 'template <int A> struct S%d { static constexpr long v = A*%d+1; };\n' "$i" "$i"
    printf 'long f%d(long x) { return x + S%d<%d>::v; }\n' "$i" "$i" "$((i % 97))"
    i=$((i + 1))
  done
  echo 'int main() { return 0; }'
} > heavy/tests/slowbuild.cpp

out=$(cd heavy && "$MCPP" test --build-timeout 1 2>&1)
echo "$out" | grep -q "build timeout after 1s" \
    || { echo "$out" | head -20; fail "--build-timeout did not stop the slow compile"; }
echo "  ok: --build-timeout bounds a slow compile and names itself"

# Argument validation is shared by all three deadlines.
out=$("$MCPP" test -p fast --build-timeout abc 2>&1)
echo "$out" | grep -q "invalid --build-timeout" || fail "bad --build-timeout accepted"

# ── 5. --workspace-timeout stops the fan-out and reports what did not run ─
# Its own fixture, with the slow member FIRST: the deadline is checked before
# each member starts, so it can only skip anything if time has already been
# spent. (In the main fixture `fast` runs first and finishes in milliseconds.)
mkdir -p wst/one/tests wst/two/tests
cat > wst/mcpp.toml <<'EOF'
[workspace]
members = ["one", "two"]
EOF
for m in one two; do
  printf '[package]\nname = "wst_%s"\nversion = "0.1.0"\n' "$m" > "wst/$m/mcpp.toml"
done
cp slow/tests/hang.cpp wst/one/tests/hang.cpp
cp fast/tests/quick.cpp wst/two/tests/quick.cpp

out=$(cd wst && "$MCPP" test --workspace --timeout 3 --workspace-timeout 1 2>&1)
rc=$?
echo "$out" | grep -q "member 'two'" \
    && { echo "$out"; fail "--workspace-timeout did not stop the fan-out"; }
echo "$out" | grep -q "not run (--workspace-timeout 1s reached)" \
    || { echo "$out"; fail "--workspace-timeout did not report skipped members"; }
[ "$rc" -ne 0 ] || fail "workspace timeout returned 0"
echo "  ok: --workspace-timeout stops the fan-out and names what did not run"

# ── 6. JSON contract: no stray lines, member-qualified, workspace summary ─
out=$("$MCPP" test --workspace --timeout 2 --message-format json 2>/dev/null)
first=$(printf '%s\n' "$out" | head -1)
case "$first" in
    '{'*) ;;
    *) echo "$out" | head -3; fail "first NDJSON line is not JSON (stdout polluted)" ;;
esac
printf '%s\n' "$out" | grep -q '"member":"fast"' \
    || { echo "$out"; fail "test records are not member-qualified"; }
printf '%s\n' "$out" | grep -q '"workspace_summary"' \
    || { echo "$out"; fail "no workspace_summary record"; }
printf '%s\n' "$out" | grep -q '"failed_members":\["slow"\]' \
    || { echo "$out"; fail "workspace_summary does not name the failed member"; }
echo "  ok: JSON stream is clean, member-qualified and has a workspace summary"

echo "PASS: 178_test_observability"
