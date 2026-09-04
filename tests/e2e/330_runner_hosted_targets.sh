#!/usr/bin/env bash
# requires: unix-shell
# 330_runner_hosted_targets.sh — `[target.<triple>].runner` on a hosted target,
# and what `mcpp run` / `mcpp test` say when this host cannot execute an
# artifact (#544).
#
# Design: .agents/docs/2026-09-02-runner-beyond-baremetal-design.md, §11 lists
# the criteria this file implements. Two properties of the setup carry the
# whole test:
#
#   1. No emulator is needed and none is used. The runner is a shell script
#      that records its argv and then executes it, so "the artifact went
#      through the runner" is asserted on the exact argv, not on a message.
#   2. The "artifact this host cannot execute" is a real host binary whose ELF
#      e_machine is patched to 0xffff AFTER the build. No binfmt_misc entry
#      and no native loader accepts it, so posix_spawnp answers ENOEXEC on
#      every Linux host — one with qemu-user registered included — and the
#      criterion does not depend on which machine runs it.
#
# `requires: unix-shell` and nothing else, on purpose: a `requires: gcc` or
# `requires: llvm` guard skips on both CI shards, and a skip exits 0.
# The ELF half is Linux-only (macOS artifacts are Mach-O); the runner half
# runs on every POSIX host.
set -uo pipefail

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $*"; exit 1; }
MCPP="${MCPP:?set MCPP to the mcpp binary under test}"

"$MCPP" new app >/dev/null 2>&1 || fail "mcpp new"
cd app
mkdir -p tests
rm -f tests/*.cpp   # the scaffold's own smoke test; the counts below are exact
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("ARTIFACT-RAN"); return 0; }
EOF
cat > tests/one.cpp <<'EOF'
int main() { return 0; }
EOF

# The host triple as the manifest spells it: the CANONICAL form, which is the
# name of the output directory (`target/<triple>/…`) and the key every
# `[target.<triple>]` reader resolves. Read from the engine rather than
# guessed. Not the "Target … → …" status line: on macOS that prints the
# driver's own spelling (`arm64-apple-darwin24.6.0`), which is not the key —
# and the first version of this test used it and failed only on macOS.
out=$("$MCPP" build 2>&1) || fail "initial build: $out"
HOST=$(ls target | head -1)
[[ -n "$HOST" ]] || fail "could not determine the host triple from target/: $out"

cat > "$TMP/runner.sh" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >> "$RUNNER_LOG"
exec "$@"
EOF
chmod +x "$TMP/runner.sh"
export RUNNER_LOG="$TMP/runner.log"

# ── 1. a declared runner is used on a hosted target, on both `run` doors ────
printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner.sh" >> mcpp.toml
out=$("$MCPP" run 2>&1) || fail "run through runner: $out"
grep -q "ARTIFACT-RAN" <<<"$out" || fail "artifact output missing: $out"
[[ -s "$RUNNER_LOG" ]] || fail "runner was not invoked (declared under [target.$HOST]): $out"
exe=$(head -1 "$RUNNER_LOG")
[[ "$exe" == */bin/app ]] || fail "runner argv[0] is not the artifact: $exe"
: > "$RUNNER_LOG"
out=$("$MCPP" run 2>&1) || fail "second run (fast path): $out"
[[ -s "$RUNNER_LOG" ]] || fail "the run fast path bypassed the declared runner: $out"

# ── 2. --no-runner executes directly and says so ───────────────────────────
: > "$RUNNER_LOG"
out=$("$MCPP" run --no-runner 2>&1) || fail "--no-runner: $out"
grep -q "ARTIFACT-RAN" <<<"$out" || fail "--no-runner lost the program output: $out"
grep -q "no-runner" <<<"$out" || fail "--no-runner printed no note: $out"
[[ ! -s "$RUNNER_LOG" ]] || fail "--no-runner still invoked the runner"

# ── 3. runner not found: names the program and the search list, exit 2 ─────
sed -i.bak "s|^runner = .*|runner = [\"mcpp-e2e-no-such-runner\"]|" mcpp.toml
out=$("$MCPP" run 2>&1); rc=$?
[[ $rc -eq 2 ]] || fail "missing runner: exit $rc, want 2: $out"
grep -q "runner 'mcpp-e2e-no-such-runner' for '$HOST' was not found" <<<"$out" \
    || fail "missing runner not named: $out"
grep -q "Searched:" <<<"$out" || fail "search list missing: $out"
grep -q "ARTIFACT-RAN" <<<"$out" && fail "artifact ran without its runner: $out"
# `mcpp test`, one test: every test is not-run with that reason, exit 2.
out=$("$MCPP" test 2>&1); rc=$?
[[ $rc -eq 2 ]] || fail "test with missing runner: exit $rc, want 2: $out"
grep -qE "NOT RUN\. 0 passed; 0 failed; 1 not run \(runner 'mcpp-e2e-no-such-runner'" <<<"$out" \
    || fail "test summary with missing runner: $out"
grep -q "one ... not run" <<<"$out" || fail "per-test not-run line missing: $out"
# --no-runner on `test` runs it directly.
out=$("$MCPP" test --no-runner 2>&1) || fail "test --no-runner: $out"
grep -qE "ok\. 1 passed; 0 failed; finished" <<<"$out" || fail "test --no-runner summary: $out"

# ── 4. no runner declared, artifact this host cannot load (ELF only) ───────
if [[ "$(uname -s)" == Linux ]]; then
  sed -i.bak "/^\[target\.$HOST\]/,/^runner = /d" mcpp.toml
  grep -q "runner" mcpp.toml && fail "runner key still present after removal"
  "$MCPP" build >/dev/null 2>&1 || fail "rebuild without runner"
  bin=$(ls target/*/*/bin/app | head -1)
  [[ -f "$bin" ]] || fail "no artifact at target/*/*/bin/app"
  printf '\xff\xff' | dd of="$bin" bs=1 seek=18 conv=notrunc status=none
  # 126, NOT 2, AND THE BAND IS THE POINT.
  #
  # `mcpp run` now reports the program's own exit status, so mcpp's answers had
  # to move out of the range a program owns. A refused spawn lands in the band
  # `env`, `timeout` and every shell already use: 127 not found, 126 found and
  # not executable, 125 anything else. This artifact has a wrecked e_machine,
  # so the kernel answers ENOEXEC and the code is 126.
  #
  # mcpp's own refusals BEFORE a spawn is attempted — no binary target, no
  # runner declared, runner program not on PATH — keep exit 2, which is what
  # every other mcpp command uses for "cannot do what was asked". The three
  # assertions above this one cover that half and are deliberately unchanged.
  out=$("$MCPP" run 2>&1); rc=$?
  [[ $rc -eq 126 ]] || fail "unrunnable artifact: exit $rc, want 126: $out"
  grep -q "this host cannot execute" <<<"$out" || fail "unrunnable message missing: $out"
  grep -q "Exec format error" <<<"$out" || fail "the kernel's answer is missing: $out"
  grep -q "\[target.$HOST\]" <<<"$out" || fail "paste-able key missing: $out"
  grep -q 'runner = \["qemu-aarch64-static"\]' <<<"$out" || fail "runner example missing: $out"
  grep -q -- "--no-runner" <<<"$out" || fail "escape hatch not mentioned: $out"
  # The criterion measured the patched artifact, not a rebuilt one.
  [[ "$(od -An -tx1 -j18 -N2 "$bin" | tr -d ' ')" == "ffff" ]] \
      || fail "the artifact was rebuilt under the run; the criterion measured nothing"

  # `mcpp test`: one test (streaming path) and two tests (capturing path).
  # Today they report a spawn failure differently; the assertion is the same.
  for n in 1 2; do
    if [[ $n -eq 2 ]]; then
      cat > tests/two.cpp <<'EOF'
int main() { return 0; }
EOF
    fi
    # A patched binary is newer than its source, so ninja keeps it; the
    # sources are touched so this iteration measures freshly built binaries.
    touch tests/*.cpp
    out=$("$MCPP" test 2>&1) || fail "test build+run before patching ($n): $out"
    grep -qE "ok\. $n passed; 0 failed" <<<"$out" || fail "sanity: tests should pass unpatched ($n): $out"
    tbins=$(find target -type f -perm -u+x \( -name one -o -name two \))
    [[ $(wc -l <<<"$tbins") -eq $n ]] || fail "expected $n test binaries, found: $tbins"
    for t in $tbins; do
      printf '\xff\xff' | dd of="$t" bs=1 seek=18 conv=notrunc status=none
    done
    out=$("$MCPP" test 2>&1); rc=$?
    [[ $rc -eq 2 ]] || fail "test unrunnable ($n): exit $rc, want 2: $out"
    grep -qE "NOT RUN\. 0 passed; 0 failed; $n not run \(this host cannot execute $HOST artifacts: Exec format error" <<<"$out" \
        || fail "test summary ($n): $out"
    [[ $(grep -c " \.\.\. not run" <<<"$out") -eq $n ]] || fail "per-test not-run lines ($n): $out"
    # The reason is printed once when established, and once in the summary.
    [[ $(grep -c "cannot execute $HOST artifacts" <<<"$out") -eq 2 ]] \
        || fail "reason printed other than once + summary ($n): $out"
    for t in $tbins; do
      [[ "$(od -An -tx1 -j18 -N2 "$t" | tr -d ' ')" == "ffff" ]] \
          || fail "test binary was rebuilt under the run ($n)"
    done
    jout=$("$MCPP" test --message-format json 2>/dev/null); jrc=$?
    [[ $jrc -eq 2 ]] || fail "json exit ($n): $jrc, want 2"
    [[ $(grep -c '"status":"not_run"' <<<"$jout") -eq $n ]] || fail "json not_run records ($n): $jout"
    grep -q "\"not_run\":$n," <<<"$jout" || fail "json summary not_run ($n): $jout"
    grep -q '"not_run_reason":"this host cannot execute' <<<"$jout" || fail "json summary reason ($n): $jout"
  done
fi

# ── 5. an array-valued typo is reported, and `runner` is in the list ───────
printf '\n[target.%s]\nrunnerX = ["x"]\n' "$HOST" >> mcpp.toml
out=$("$MCPP" build 2>&1)
grep -q "unsupported key 'runnerX'" <<<"$out" || fail "array typo not reported: $out"
grep -q "Supported keys: cxx_runtime, linkage, runner, sysroot, toolchain" <<<"$out" \
    || fail "runner missing from the supported-keys list: $out"

echo "PASS: 330_runner_hosted_targets"
