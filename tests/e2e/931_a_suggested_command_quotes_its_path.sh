#!/usr/bin/env bash
# requires: unix-shell fresh-sandbox
# A command printed for a person to paste quotes the paths in it (mcpp#832,
# R1). A home under `/tmp/mc probe` printed
# `mcpp self init --force --home /tmp/mc probe/home`, which selects `/tmp/mc`
# and hands `probe/home` to the parser.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

export HOME="$work/user"
mkdir -p "$HOME"
unset MCPP_HOME
first="$work/first home"
second="$work/second home"
"$MCPP" self init --home "$first" >/dev/null 2>&1 || fail "the first home could not be initialised"
out="$("$MCPP" self init --home "$second" 2>&1 || true)"
grep -qF -- "--home '$second'" <<<"$out" || fail "the suggested command does not quote the spaced path"
cmd=$(sed -n "s/.*use \`\(mcpp self init --force --home .*\)\` to select.*/\1/p" <<<"$out")
[[ -n "$cmd" ]] || fail "no suggested command found"
eval "\"$MCPP\" ${cmd#mcpp }" >/dev/null 2>&1 || fail "the suggested command, pasted, failed"
echo "PASS: a suggested command quotes the path it carries, and pastes"
