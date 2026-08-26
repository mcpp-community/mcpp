#!/usr/bin/env bash
# requires: unix-shell jq
# A target request that declines to name a C library resolves to a SUPPORTED row.
#
# ⭐⭐ `parse` FILLS THE ENV SEGMENT LEXICALLY, AND THE TIER GATE USED TO ASK
# ABOUT THE FILLED VALUE RATHER THAN ABOUT THE REQUEST.
#
# The fill exists so the IDENTITY stays total: `x86_64-linux` IS
# `x86_64-linux-gnu`, that is the output directory and the cache key, and a unit
# test pins it. What it is not is an answer to "does mcpp support this".
#
# ⚠️ MEASURED ON 2026.8.26.1, same machine, same graph, two spellings:
#
#     $ mcpp build --target aarch64-linux
#       error: target 'aarch64-linux-gnu' is registered but not yet supported
#     $ mcpp build --target aarch64-linux-musl
#       Finished dev [unoptimized + debuginfo] in 0.99s
#
# The question asked was "aarch64, Linux". The question answered was
# "aarch64-linux-GNU" — and the refusal quotes a triple the reader never typed.
# `examples/06-openkal-cross` teaches the short spelling for three platforms;
# the fourth was the one that could not be written.
#
# ⭐⭐ BOTH DIRECTIONS, BECAUSE "SEND EVERY BARE -linux TO musl" ALSO FIXES
# aarch64 AND WOULD BREAK EVERY PROJECT ON THE PLANET. Half two is the control:
# `x86_64-linux` must still be gnu, because gnu is a supported row there.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf '[package]\nname    = "reqprobe"\nversion = "0.1.0"\n' > mcpp.toml
printf 'extern "C" int main(int, char**, char**) { return 0; }\n' > src/main.cpp

# ⭐ CLASSIFICATION FROM THE MACHINE INTERFACE. `data.reason` is a finite token;
# a substring search over prose stops asserting the moment the prose is reworded
# — measured in this repo, twice, in one session.
reason_for() {
    "$MCPP" why toolchain --target "$1" --format json 2>/dev/null \
        | jq -r '.data.reason // "-"' | tr -d '\r'
}

# ⚠️⚠️ THE CLAIM IS ABOUT COMPLETION, NOT ABOUT SERVABILITY, AND ONLY ONE OF
# THOSE IS THE SAME ON EVERY BUILD HOST.
#
# `aarch64-linux-musl` pins the musl-gcc payload. macOS has no gcc payload at
# all, so this row legitimately answers `host-cannot-serve` there — a DIFFERENT
# question, correctly answered, and a test that demanded `none` would have gone
# red on two of the four hosts for a reason unrelated to what it is checking.
#
# What is host-independent is which ROW the request resolved to, and that is
# visible either way: on success in `data.triple`, and under a refusal in the
# first line of the message, which names its subject.
#
# ⚠️ THE SUBJECT, NOT THE DOCUMENT. Grepping the whole JSON was the first draft
# and it is contaminated: `host-cannot-serve` lists every target this host CAN
# serve, so `x86_64-linux-musl` appears in a message that is about something
# else entirely. The list is an answer to a different question sitting in the
# same string.
resolved_row() {   # request → the row it resolved to
    local doc t
    doc="$("$MCPP" why toolchain --target "$1" --format json 2>/dev/null | tr -d '\r')"
    t="$(printf '%s' "$doc" | jq -r '.data.triple.llvm // empty')"
    if [ -n "$t" ]; then printf '%s' "$t"; return; fi
    printf '%s' "$doc" | jq -r '.diagnostics[0].message // empty' \
        | head -1 | sed -n "s/^[^']*'\\([^']*\\)'.*/\\1/p"
}
message_of() {
    "$MCPP" why toolchain --target "$1" --format json 2>/dev/null \
        | jq -r '.diagnostics[].message' | tr -d '\r'
}

# ── Half one: the short spelling reaches the row that exists ──────────────
r="$(reason_for aarch64-linux)"
row="$(resolved_row aarch64-linux)"
case "$r" in
  tier-planned|unknown-target)
    echo "FAIL: the tier gate still answers about the lexical fill (reason '$r')"
    echo "      aarch64-linux-musl is 'verified'; aarch64-linux-gnu is 'planned'"
    message_of aarch64-linux | sed 's/^/        /'
    exit 1 ;;
  none)
    echo "  ok  --target aarch64-linux resolves instead of refusing" ;;
  host-cannot-serve)
    echo "  ok  --target aarch64-linux completed (this host serves no such payload)" ;;
  *)
    echo "FAIL: --target aarch64-linux refused for reason '$r'"
    message_of aarch64-linux | sed 's/^/        /'
    exit 1 ;;
esac

# ⚠️ AND THE ROW IS THE IDENTITY, NOT THE SPELLING. The output directory is the
# identity. Asserting only "it did not refuse with tier-planned" would stay
# green in a world where the completion picked some other row entirely.
case "$row" in
  *-musl) echo "  ok  and it resolved to the musl row ($row)" ;;
  *)      echo "FAIL: aarch64-linux resolved to '$row', not a musl row"; exit 1 ;;
esac
case "$row" in
  aarch64*) ;;
  *) echo "FAIL: aarch64-linux resolved to '$row', which is not aarch64"; exit 1 ;;
esac

# ── Half two: the control ────────────────────────────────────────────────
#
# x86_64-linux-gnu is `verified`, so the lexical fill names a supported row and
# nothing may move. This is the assertion that a fix aimed at aarch64 did not
# redefine what `-linux` means everywhere — and it is the half that would catch
# "send every bare -linux to musl", which passes half one perfectly.
hostr="$(reason_for x86_64-linux)"
hostrow="$(resolved_row x86_64-linux)"
case "$hostrow" in
  *-gnu) echo "  ok  and x86_64-linux is still gnu ($hostrow, reason '$hostr')" ;;
  *)     echo "FAIL: x86_64-linux resolved to '$hostrow' (reason '$hostr')"; exit 1 ;;
esac

# ── Half three: a written segment is a request, not a gap ────────────────
#
# The escape hatch. Someone who wants the `planned` row writes it out, and the
# tier gate then refuses a string that IS in their command.
wr="$(reason_for aarch64-linux-gnu)"
if [ "$wr" = tier-planned ]; then
    echo "  ok  and writing -gnu still opts into the planned row's refusal"
else
    echo "FAIL: --target aarch64-linux-gnu gave reason '$wr', expected tier-planned"
    exit 1
fi

echo "OK: a request that named no C library resolves to a row that exists"
