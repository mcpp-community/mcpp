#!/usr/bin/env bash
# requires: unix-shell jq
# "unknown target" is a claim about the vocabulary, and it was false for a whole
# arch+os family.
#
# MEASURED ON 2026.8.26.1:
#
#     $ mcpp why toolchain --target riscv64-linux --format json
#       unknown target 'riscv64-linux'
#       "reason": "other"
#
# `riscv64-linux-musl` is in `kKnownTargets` as `planned`. The lexical env fill
# had produced `riscv64-linux-gnu` — a row that genuinely does not exist — and
# the gate reported on the fill. So a registered family was called unknown, and
# the refusal carried no code at all: `other` is what the machine interface
# prints for a branch nobody named, and this branch had a perfectly good name.
#
# TWO ASSERTIONS, BECAUSE THE WORD AND THE CODE FAIL SEPARATELY. A message
# fixed without a code still reports `other`; a code added without fixing the
# message still tells the reader their target does not exist.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf '[package]\nname    = "unkprobe"\nversion = "0.1.0"\n' > mcpp.toml
printf 'extern "C" int main(int, char**, char**) { return 0; }\n' > src/main.cpp

json_for() { "$MCPP" why toolchain --target "$1" --format json 2>/dev/null; }

# ── Half one: registered families ─────────────────────────────────────────
# `riscv64-linux`, whose one row is riscv64-linux-musl (preview since the row
# was measured), resolves to that row; whatever the answer is, it is not
# `unknown-target` and it quotes no triple the reader never typed.
j="$(json_for riscv64-linux)"
reason="$(printf '%s' "$j" | jq -r '.data.reason // "-"' | tr -d '\r')"
msg="$(printf '%s' "$j" | jq -r '.diagnostics[]?.message' | tr -d '\r')"

if [ "$reason" = unknown-target ] || [ "$reason" = tier-planned ]; then
    echo "FAIL: riscv64-linux refused as '$reason'"
    printf '%s\n' "$msg" | sed 's/^/        /'
    exit 1
fi
echo "  ok  a registered family is not refused as unknown"

# AND IT DOES NOT QUOTE A TRIPLE THE READER NEVER TYPED. The old message was
# about `riscv64-linux-gnu`, a string that appears nowhere in the command and
# nowhere in the vocabulary.
if printf '%s\n' "$msg" | grep -q 'riscv64-linux-gnu'; then
    echo "FAIL: the answer quotes 'riscv64-linux-gnu', which the reader never wrote"
    printf '%s\n' "$msg" | sed 's/^/        /'
    exit 1
fi
echo "  ok  and it does not quote a triple nobody wrote"

# A family whose rows are planned (`i686-linux`) refuses as planned, and the
# message names the row the request resolves to: telling someone their target
# is planned is only actionable once they can see which spelling is registered.
jp="$(json_for i686-linux)"
reasonp="$(printf '%s' "$jp" | jq -r '.data.reason // "-"' | tr -d '\r')"
msgp="$(printf '%s' "$jp" | jq -r '.diagnostics[]?.message' | tr -d '\r')"
if [ "$reasonp" != tier-planned ]; then
    echo "FAIL: i686-linux refused as '$reasonp', expected tier-planned"
    printf '%s\n' "$msgp" | sed 's/^/        /'
    exit 1
fi
if printf '%s\n' "$msgp" | grep -q 'i686-linux-gnu'; then
    echo "  ok  a planned family refuses as planned and names its row"
else
    echo "FAIL: the refusal does not name i686-linux-gnu"
    printf '%s\n' "$msgp" | sed 's/^/        /'
    exit 1
fi

# ── Half two: a genuine typo is still unknown, and now carries its code ──
j2="$(json_for x86_64-linux-mus)"
reason2="$(printf '%s' "$j2" | jq -r '.data.reason // "-"' | tr -d '\r')"
msg2="$(printf '%s' "$j2" | jq -r '.diagnostics[].message' | tr -d '\r')"

if [ "$reason2" != unknown-target ]; then
    echo "FAIL: a typo'd triple refused as '$reason2', expected unknown-target"
    printf '%s\n' "$msg2" | sed 's/^/        /'
    exit 1
fi
echo "  ok  a typo is still unknown, and the refusal now carries that code"

if printf '%s\n' "$msg2" | grep -q 'x86_64-linux-musl'; then
    echo "  ok  and the suggestion survived"
else
    echo "FAIL: the did-you-mean suggestion was lost"
    printf '%s\n' "$msg2" | sed 's/^/        /'
    exit 1
fi

echo "OK: a registered family is not reported unknown"
