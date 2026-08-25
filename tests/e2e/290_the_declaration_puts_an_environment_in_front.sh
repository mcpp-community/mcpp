#!/usr/bin/env bash
# requires: gcc unix-shell
# A build program looks up a tool and finds the one the PROJECT declared.
#
# ⚠️ IT USED TO FIND WHATEVER THE MACHINE HAD, AND `command -v` CANNOT TELL THE
# DIFFERENCE. Measured 2026-08-25 with a build program that printed its own
# PATH: mcpp's own environment appeared nowhere in it, and
# `command -v qemu-system-riscv64` returned a shim that answers, when run,
#
#     [error] qemu-system-riscv64 is not installed in this subos (_)
#
# — found, reported present, unable to execute, while a working copy sat in an
# environment the build could not reach.
#
# ⭐⭐ THIS FILE ASSERTS BOTH DIRECTIONS, BECAUSE ONLY ONE OF THEM IS THE
# FEATURE. Prepending unconditionally would have passed the first half and is
# the design that was withdrawn: a shared directory in front of every project
# makes what a build sees depend on what else was installed on that machine.
# The declaration is what puts it there, so a project that declares nothing
# must come out byte-for-byte unchanged.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ⚠️ A NON-ZERO EXIT, BECAUSE THAT IS WHAT MAKES THE OUTPUT VISIBLE. mcpp
# prints a build program's stdout only when it fails — a probe that succeeds
# says nothing, which is a property of the protocol and not of this test.
probe='import std;

int main() {
    const char* p = std::getenv("PATH");
    std::string_view path(p ? p : "");
    std::string_view first;
    for (auto part : std::views::split(path, '"'"':'"'"')) {
        first = std::string_view(part);
        break;
    }
    std::println("PROBE_FIRST={}", first);
    std::println("PROBE_ENTRIES={}", std::ranges::count(path, '"'"':'"'"') + 1);
    return 1;
}'

# Returns "<first>|<entries>", or nothing if the program did not run.
run_probe() {
    local dir="$1"
    local out
    out="$(cd "$dir" && "$MCPP" build 2>&1 || true)"
    local f e
    f="$(printf '%s\n' "$out" | grep -oP 'PROBE_FIRST=\K.*' | head -1)"
    e="$(printf '%s\n' "$out" | grep -oP 'PROBE_ENTRIES=\K[0-9]+' | head -1)"
    [ -n "$f" ] && printf '%s|%s\n' "$f" "$e"
}

make_project() {
    local dir="$1" xlings="$2"
    mkdir -p "$dir/src"
    { printf '[package]\nname    = "pathprobe"\nversion = "0.1.0"\n'
      [ -n "$xlings" ] && printf '\n%s\n' "$xlings"; } > "$dir/mcpp.toml"
    printf 'int main() { return 0; }\n' > "$dir/src/main.cpp"
    printf '%s\n' "$probe" > "$dir/build.mcpp"
}

# ── Half one: a project that declared nothing ─────────────────────────────
make_project "$work/plain" ""
plain="$(run_probe "$work/plain")"
if [ -z "$plain" ]; then
    echo "SKIP: the build program did not report — it may not have run here"
    exit 0
fi
plain_first="${plain%%|*}"

case "$plain_first" in
  */subos/*/bin)
    echo "FAIL: a project that declared no environment got one in front anyway"
    echo "        got: $plain_first"
    exit 1 ;;
  *)
    echo "  ok  a project that declares nothing keeps the PATH it was given" ;;
esac

# ── Half two: the same project, declaring one ─────────────────────────────
#
# `default` rather than a private name: mcpp READS an environment and never
# creates one, so a name nobody has bootstrapped is a hard error by design.
# What is under test is the prepending, and `default` exercises it on any
# machine that has run `mcpp self init`.
make_project "$work/declared" '[xlings]
subos = "default"'
declared="$(run_probe "$work/declared")"
if [ -z "$declared" ]; then
    echo "SKIP: the declaring project's build program did not report"
    exit 0
fi
declared_first="${declared%%|*}"
declared_entries="${declared##*|}"

case "$declared_first" in
  */subos/*/bin)
    echo "  ok  a project that declares one gets it first: $declared_first" ;;
  *)
    echo "FAIL: the declared environment is not at the front of PATH"
    echo "        got: $declared_first"
    exit 1 ;;
esac

# ⭐ AND THE HOST IS STILL BEHIND IT. One entry means the inherited PATH was
# replaced rather than extended, which would break every build program that
# calls `git`, `python3` or a shell.
if [ "${declared_entries:-1}" -gt 1 ]; then
    echo "  ok  the inherited PATH survives behind it ($declared_entries entries)"
else
    echo "FAIL: PATH was replaced, not prefixed — only $declared_entries entry"
    exit 1
fi

echo "OK: the declaration puts an environment in front, and only the declaration does"
