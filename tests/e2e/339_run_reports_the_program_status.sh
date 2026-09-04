#!/usr/bin/env bash
# requires: gcc unix-shell
#
# `mcpp run` REPORTS THE PROGRAM'S OWN EXIT STATUS.
#
# It used to fold every non-zero status to 1 so that 2 could mean "could not
# start". The distinction was worth keeping; the price was not. Measured before
# the change: a program whose `main` returns 3 made `mcpp run` exit 1, and a
# bare-metal image qemu reported as 3 arrived as 1 as well. A command that
# cannot report a status cannot be used in a script, which is most of what
# `mcpp run` is for.
#
# THREE BANDS, AND THE TEST COVERS ALL THREE:
#
#   0-124     the program's own status, passed through unchanged
#   125-127   the spawn was attempted and refused (127 not found,
#             126 found but not executable, 125 anything else)
#   2         mcpp refused BEFORE attempting anything — a usage or
#             configuration error, the same code every other mcpp command uses
#
# The middle band is the shell's, not this project's: `env`, `timeout` and
# `nice` already answer 126/127 with these meanings, so a reader who meets one
# does not have to look it up.
set -euo pipefail
fail() { echo "FAIL: $*"; exit 1; }
MCPP="${MCPP:?set MCPP to the mcpp binary under test}"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cd "$work"
mkdir -p src

cat > mcpp.toml <<'EOF'
[package]
name    = "status"
version = "0.1.0"

[build]
sources = ["src/main.cpp"]
EOF

status_of() {   # $1 = what main returns
  printf 'int main() { return %s; }\n' "$1" > src/main.cpp
  set +e
  "$MCPP" run > run.log 2>&1
  local rc=$?
  set -e
  echo "$rc"
}

# ── 1. the ordinary case: a status a script would branch on ───────────────
for want in 0 1 3 42 124; do
  got=$(status_of "$want")
  [[ "$got" == "$want" ]] \
    || fail "main returned $want, mcpp run exited $got (want $want)"
done
echo "  ok  0 1 3 42 124 all pass through"

# ── 2. THE BOUNDARY, BECAUSE THAT IS WHERE A BAND SPLIT GOES WRONG ────────
#
# A program is allowed to exit 125-127 too. mcpp cannot distinguish that from
# its own answer by the number alone, and does not try: what separates them is
# that a launcher failure always prints a reason to stderr and a program's own
# status never does. Assert the number here and the silence with it, so that a
# future change which starts printing on the success path is caught.
got=$(status_of 127)
[[ "$got" == "127" ]] || fail "main returned 127, mcpp run exited $got"
grep -qiE '^error:' run.log \
  && fail "a program exiting 127 must not produce an mcpp error line: $(cat run.log)"
echo "  ok  a program may exit 127 itself, and mcpp stays silent about it"

# ── 3. the spawn band: found, not executable ──────────────────────────────
#
# A file that exists, is marked executable, and is not a program the kernel can
# load. The runner is declared so that the failure is the RUNNER's spawn rather
# than the artefact's, which keeps this case independent of the host triple.
printf 'int main() { return 0; }\n' > src/main.cpp
"$MCPP" build >/dev/null 2>&1 || fail "build for the runner cases"
HOST=$(ls target | head -1)
[[ -n "$HOST" ]] || fail "no target directory to read the host triple from"
printf 'this is not an executable format\n' > not-a-program
chmod +x not-a-program
cat >> mcpp.toml <<EOF

[target.$HOST]
runner = ["$PWD/not-a-program"]
EOF
set +e
out=$("$MCPP" run 2>&1); rc=$?
set -e
[[ $rc -eq 126 ]] || fail "unexecutable runner: exit $rc, want 126: $out"
grep -qiE '^error:' <<<"$out" || fail "a refused spawn must say why: $out"
echo "  ok  a refused spawn exits 126 and says why"

# ── 4. the spawn band: not found ──────────────────────────────────────────
sed -i.bak "s|$PWD/not-a-program|$PWD/no-such-runner-at-all|" mcpp.toml
set +e
out=$("$MCPP" run 2>&1); rc=$?
set -e
# mcpp looks a declared runner up before spawning, so this is its own refusal
# (2) rather than the kernel's (127). Both are outside 0-124, which is the
# property a script depends on; the test states which one this is so that a
# change of mind about it is visible rather than silent.
[[ $rc -eq 2 || $rc -eq 127 ]] \
  || fail "missing runner: exit $rc, want 2 (pre-flight) or 127 (spawn): $out"
echo "  ok  a missing runner exits $rc, outside the program's band"

echo "PASS: 339 mcpp run reports the program's own exit status"
