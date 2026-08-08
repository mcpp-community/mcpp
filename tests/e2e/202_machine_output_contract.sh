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

# An option this command does not have.
check "unknown option (cache list)" 2 "$MCPP" cache list --format json
check "unknown option (self env)"   2 "$MCPP" self env --format json
check "unknown option (top level)"  2 "$MCPP" --no-such-option

# A value this command does not accept. Already correct today; asserted so it
# stays that way while the option path is changed around it.
check "unknown value (pack)" 2 "$MCPP" pack --format bogus

[[ "$fail" -eq 0 ]] || exit 1
echo "PASS: usage errors stay off stdout and exit 2"
