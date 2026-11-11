#!/usr/bin/env bash
# requires: unix-shell jq
# A target request that declines to name a C library resolves to a SUPPORTED row.
#
# `parse` FILLS THE ENV SEGMENT LEXICALLY, AND THE TIER GATE USED TO ASK
# ABOUT THE FILLED VALUE RATHER THAN ABOUT THE REQUEST.
#
# The fill exists so the IDENTITY stays total: `x86_64-linux` IS
# `x86_64-linux-gnu`, that is the output directory and the cache key, and a unit
# test pins it. What it is not is an answer to "does mcpp support this".
#
# MEASURED ON 2026.8.26.1, same machine, same graph, two spellings:
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
# BOTH DIRECTIONS, BECAUSE "SEND EVERY BARE -linux TO musl" ALSO FIXES
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

# CLASSIFICATION FROM THE MACHINE INTERFACE. `data.reason` is a finite token;
# a substring search over prose stops asserting the moment the prose is reworded
# — measured in this repo, twice, in one session.
reason_for() {
    "$MCPP" why toolchain --target "$1" --format json 2>/dev/null \
        | jq -r '.data.reason // "-"' | tr -d '\r'
}

# THE CLAIM IS ABOUT COMPLETION, NOT ABOUT SERVABILITY, AND ONLY ONE OF
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
# THE SUBJECT, NOT THE DOCUMENT. Grepping the whole JSON was the first draft
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
        | head -1 | sed -n -e "s/^[^']*'\([^']*\)'.*/\1/p" \
                            -e "s/^\[target\.\([^]]*\)\].*/\1/p"
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
    echo "      aarch64-linux-gnu is verified; completion must preserve its GNU identity"
    message_of aarch64-linux | sed 's/^/        /'
    exit 1 ;;
  none)
    echo "  ok  --target aarch64-linux resolves instead of refusing" ;;
  host-cannot-serve)
    echo "  ok  --target aarch64-linux completed (this host serves no such payload)" ;;
  *)
    # THE ROW'S PAYLOAD IS A PACKAGE, AND A PACKAGE MAY NOT BE INSTALLABLE
    # WHERE THIS RUNS (2026.11.11.1, G1): the GNU row names its sysroot, and
    # an index without it -- a CI runner ahead of the index PR, an offline
    # machine -- refuses about the PACKAGE, quoting the row it resolved. That
    # refusal still answers this half's question: the request reached a row
    # of the system, which the refusal's subject and the row checks below
    # both name. Anything else it names is not about the row and stays red.
    if [ "$row" = aarch64-linux-gnu ] || [ "$row" = aarch64-unknown-linux-gnu ] \
       || [ "$row" = aarch64-linux-musl ] || [ "$row" = aarch64-unknown-linux-musl ]; then
      if message_of aarch64-linux | grep -q "could not be installed"; then
        echo "  ok  --target aarch64-linux reached a row whose payload package is not installable here"
      else
        echo "FAIL: --target aarch64-linux refused for reason '$r'"
        message_of aarch64-linux | sed 's/^/        /'
        exit 1
      fi
    else
      echo "FAIL: --target aarch64-linux refused for reason '$r'"
      message_of aarch64-linux | sed 's/^/        /'
      exit 1
    fi ;;
esac

# AND THE ROW IS THE IDENTITY, NOT THE SPELLING. The output directory is the
# identity. Asserting only "it did not refuse with tier-planned" would stay
# green in a world where the completion picked some other row entirely.
# SINCE 2026.11.9.1 THE REQUEST IS A PATTERN (SPEC-016 §4): it takes the GNU row
# where that row's provider is available -- an aarch64 Linux host -- and the
# next available row of the system elsewhere (`aarch64-linux-musl`, through the
# musl cross payload or openkal-platform), the report saying why.
hostpair="$(uname -s)-$(uname -m)"
case "$hostpair" in
  Linux-aarch64)
    case "$row" in
      aarch64-linux-gnu|aarch64-unknown-linux-gnu) echo "  ok  and it resolved to the native GNU row ($row)" ;;
      *) echo "FAIL: aarch64-linux resolved to '$row' on an aarch64 Linux host, not the GNU row"; exit 1 ;;
    esac ;;
  *)
    case "$row" in
      aarch64-linux-gnu|aarch64-unknown-linux-gnu|aarch64-linux-musl|aarch64-unknown-linux-musl)
        echo "  ok  and it resolved to a row of the system available here ($row)" ;;
      *) echo "FAIL: aarch64-linux resolved to '$row', no aarch64 Linux row"; exit 1 ;;
    esac ;;
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
case "$hostpair:$hostrow" in
  Linux-x86_64:*-gnu) echo "  ok  and x86_64-linux is still gnu on an x86_64 Linux host ($hostrow, reason '$hostr')" ;;
  Linux-x86_64:*)     echo "FAIL: x86_64-linux resolved to '$hostrow' on an x86_64 Linux host (reason '$hostr')"; exit 1 ;;
  *:x86_64*linux*)    echo "  ok  and x86_64-linux is a row of the system available here ($hostrow)" ;;
  *)                  echo "FAIL: x86_64-linux resolved to '$hostrow' (reason '$hostr')"; exit 1 ;;
esac

# ── Half three: a written segment is a request, not a gap ────────────────
#
# An explicit segment names the identity whatever the host: it is never replaced
# by another row, served here or not.
case "$(resolved_row aarch64-linux-gnu)" in
  aarch64-linux-gnu|aarch64-unknown-linux-gnu) echo "  ok  and writing -gnu keeps the GNU row" ;;
  *) echo "FAIL: written -gnu resolved to '$(resolved_row aarch64-linux-gnu)'"; exit 1 ;;
esac

echo "OK: a request that named no C library resolves to a row that exists"
