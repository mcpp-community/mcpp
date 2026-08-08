#!/usr/bin/env bash
# 202_machine_output_contract.sh — stdout belongs to the protocol.
#
# A client that asks for machine-readable output has exactly one channel for
# it. Anything mcpp says about ITSELF -- a usage error, an unsupported format
# -- has to go somewhere else, or the client's parser eats it.
#
# Measured before this test existed:
#
#   mcpp cache list --format json   ->  stdout: "Error: unknown option: --format"
#                                       stderr: (empty)   rc=1
#   mcpp pack --format bogus        ->  stderr: "invalid --format ..."  rc=2
#
# Two shapes for the same class of mistake, and the first one writes human text
# into the channel a protocol owns. `mcpp cache list --format json | jq` gets a
# parse error with nothing to distinguish "this mcpp is too old" from "the
# command failed".
#
# Design: .agents/docs/2026-08-08-machine-readable-output-protocol-design.md §2
set -uo pipefail

fail=0
check() { # description  expected_rc  cmd...
    local what=$1 want=$2; shift 2
    local out err rc
    out=$("$@" 2>/dev/null); rc=$?
    err=$("$@" 2>&1 >/dev/null)
    if [[ -n "$out" ]]; then
        echo "FAIL: $what wrote to stdout: $(echo "$out" | head -1 | cut -c1-70)"
        fail=1
    fi
    if [[ -z "$err" ]]; then
        echo "FAIL: $what said nothing on stderr"
        fail=1
    fi
    if [[ "$rc" != "$want" ]]; then
        echo "FAIL: $what exited $rc, expected $want"
        fail=1
    fi
}

# An option that does not exist. This is the path a client hits when it probes
# an older mcpp for a capability -- the most common machine-facing failure,
# and the one that used to print to stdout.
check "unknown option (cache list)" 2 "$MCPP" cache list --no-such-option
check "unknown option (self env)"   2 "$MCPP" self env --no-such-option
check "unknown option (top level)"  2 "$MCPP" --no-such-option

# A value the command does not accept. `--format` exists on these now, so this
# is the branch that answers "I know the option, not that value".
check "unsupported value (self env)"   2 "$MCPP" self env --format yaml
check "unsupported value (cache list)" 2 "$MCPP" cache list --format yaml
check "unsupported value (pack)"       2 "$MCPP" pack --format bogus

# And the supported ones must still be JSON on stdout. Asserted here because
# every check above is about what does NOT happen; without this, deleting the
# feature entirely would leave the file green.
for cmd in "self env" "cache list"; do
    out=$($MCPP $cmd --format json 2>/dev/null) || { echo "FAIL: $cmd --format json exited non-zero"; fail=1; }
    echo "$out" | grep -q '"schemaVersion"' || { echo "FAIL: $cmd --format json has no schemaVersion"; fail=1; }
    echo "$out" | grep -q '"kind"'          || { echo "FAIL: $cmd --format json has no kind"; fail=1; }
done

[[ "$fail" -eq 0 ]] || exit 1
echo "PASS: usage errors stay off stdout and exit 2"
