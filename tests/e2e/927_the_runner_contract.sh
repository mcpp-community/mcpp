#!/usr/bin/env bash
# requires: gcc unix-shell
# The runner contract and `run_under` (design 2026-10-23 §3.7, §4.7, S11).
#
# An artifact runs as `<run_under…> <runner…> <artifact> <args>`. The engine
# knows no runner by name; what it owes every one of them is the same argv,
# the same environment (`MCPP_RUN_*`), a closure manifest a runner that moves
# the artifact can read, and the program's own exit status back. A fake runner
# records all of it.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_toolchain_env.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

mkdir -p "$work/app/src" "$work/bin"
cd "$work/app"
cat > "$work/bin/via-host" <<'SH'
#!/bin/sh
{
  echo "argv: $*"
  echo "triple=$MCPP_RUN_TRIPLE kind=$MCPP_RUN_KIND mode=$MCPP_RUN_MODE"
  echo "id=$MCPP_RUN_ID"
  echo "closure=$MCPP_RUN_CLOSURE"
} >> "$RECORD"
shift   # the host name
exec "$@"
SH
chmod +x "$work/bin/via-host"
export PATH="$work/bin:$PATH" RECORD="$work/record"
cat > src/main.cpp <<'CPP'
int main(int argc, char**) { return argc == 2 ? 7 : 3; }
CPP
triple="$(uname -m)-linux-gnu"
manifest() {
    cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"
[toolchain]
default = "gcc@${GCC_VERSION}"
[target.$triple]
$1
TOML
}

# Stated in the project.
manifest 'run_under = ["via-host", "team-mac"]'
rm -f "$RECORD"
set +e; out="$("$MCPP" run -- one 2>&1)"; rc=$?; set -e
[[ $rc -eq 7 ]] || fail "the program's exit status did not come back through run_under (got $rc)"
grep -q "Via via-host team-mac  \[.*mcpp.toml:7\]" <<<"$out" || fail "no Via line naming where run_under was stated"
grep -q "argv: team-mac .*/bin/app one" "$RECORD" || fail "run_under was not composed in front of the artifact"
grep -q "triple=$triple kind=exe mode=run" "$RECORD" || fail "the contract variables are missing"
closure=$(sed -n 's/^closure=//p' "$RECORD")
[[ -f "$closure" ]] || fail "MCPP_RUN_CLOSURE names no file"
grep -q '"schema": 1' "$closure" || fail "the closure manifest carries no schema"
grep -q '"artifact": ".*/bin/app"' "$closure" || fail "the closure manifest does not name the artifact"

# Cleared on this machine, and dropped by --no-runner.
rm -f "$RECORD"
var="MCPP_TARGET_$(echo "$triple" | tr 'a-z-' 'A-Z_')_RUN_UNDER"
set +e; env "$var=none" "$MCPP" run -- one >/dev/null 2>&1; rc=$?; set -e
[[ $rc -eq 7 && ! -f "$RECORD" ]] || fail "\`..._RUN_UNDER=none\` did not clear run_under"
set +e; "$MCPP" run --no-runner -- one >/dev/null 2>&1; rc=$?; set -e
[[ $rc -eq 7 && ! -f "$RECORD" ]] || fail "--no-runner did not drop run_under"

# Stated as this machine's default.
manifest ''
home="${MCPP_HOME:-$HOME/.mcpp}"
cp "$home/config.toml" "$work/config.bak"
trap 'cp "$work/config.bak" "$home/config.toml"; rm -rf "$work"' EXIT
printf '\n[target.%s]\nrun_under = ["via-host", "my-vm"]\n' "$triple" >> "$home/config.toml"
rm -f "$RECORD"
set +e; out="$("$MCPP" run -- one 2>&1)"; rc=$?; set -e
cp "$work/config.bak" "$home/config.toml"
[[ $rc -eq 7 ]] || fail "config.toml's run_under failed (got $rc)"
grep -q "argv: my-vm " "$RECORD" || fail "config.toml's [target.<triple>].run_under was not the default"

# A program nobody installed is named before anything runs.
manifest 'run_under = ["no-such-wrapper", "x"]'
set +e; out="$("$MCPP" run 2>&1)"; rc=$?; set -e
[[ $rc -eq 2 ]] || fail "a missing run_under program did not fail with 2 (got $rc)"
grep -q "no-such-wrapper" <<<"$out" || fail "the missing program was not named"

# Tests run through it too.
manifest 'run_under = ["via-host", "team-mac"]'
mkdir -p tests
cat > tests/t.cpp <<'CPP'
int main() { return 0; }
CPP
rm -f "$RECORD"
out="$("$MCPP" test 2>&1)" || fail "mcpp test through run_under failed"
grep -q "mode=test" "$RECORD" || fail "a test was not run through run_under with MCPP_RUN_MODE=test"

echo "PASS: run_under composes in front of the runner and the contract reaches it"
