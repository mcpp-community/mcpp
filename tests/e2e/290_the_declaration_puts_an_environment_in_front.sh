#!/usr/bin/env bash
# requires: gcc unix-shell
# A build program looks up a tool and finds the one the PROJECT declared.
#
# ⚠️ IT USED TO FIND WHATEVER THE MACHINE HAD, AND `command -v` CANNOT TELL THE
# DIFFERENCE. Measured 2026-08-25 with a build program that printed its own
# PATH: the project's declared environment appeared nowhere in it, and
# `command -v qemu-system-riscv64` returned a shim that answers, when run,
#
#     [error] qemu-system-riscv64 is not installed in this subos (_)
#
# — found, reported present, unable to execute, while a working copy sat in an
# environment the build could not reach.
#
# ⭐⭐ THIS FILE ASSERTS BOTH DIRECTIONS, BECAUSE ONLY ONE OF THEM IS THE
# FEATURE. Prepending unconditionally would pass the "declared" half, and that
# is the design that was withdrawn: a shared directory in front of every
# project makes what a build sees depend on what else was installed on that
# machine. The declaration is what puts it there, so a project that declares
# nothing must come out unchanged.
#
# ⚠️⚠️ AND "UNCHANGED" IS COMPARED AGAINST THE INHERITED VALUE, NOT AGAINST A
# PATTERN. The first version of this half rejected a first entry matching
# `*/subos/*/bin` — and CI's own PATH already begins with one, because the
# runner activates an xlings environment to get mcpp at all:
#
#     MCPP: /home/runner/.xlings/subos/default/bin/mcpp
#
# so it reported mcpp prepending something that was already there. A test for
# "did not change it" has to hold the before and the after side by side.
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
    std::println("PROBE_PATH={}", p ? p : "");
    return 1;
}'

# Echoes the child's PATH, or nothing if the build program did not run.
run_probe() {
    local dir="$1" out
    out="$(cd "$dir" && "$MCPP" build 2>&1 || true)"
    printf '%s\n' "$out" | grep -oP 'PROBE_PATH=\K.*' | head -1
}

make_project() {
    local dir="$1" xlings="$2"
    mkdir -p "$dir/src"
    { printf '[package]\nname    = "pathprobe"\nversion = "0.1.0"\n'
      [ -n "$xlings" ] && printf '\n%s\n' "$xlings"; } > "$dir/mcpp.toml"
    printf 'int main() { return 0; }\n' > "$dir/src/main.cpp"
    printf '%s\n' "$probe" > "$dir/build.mcpp"
}

# The value every assertion below is relative to. mcpp inherits this shell's
# PATH, so this is exactly what an unchanged child would report.
inherited="$PATH"

# ── Half one: a project that declared nothing ─────────────────────────────
make_project "$work/plain" ""
plain="$(run_probe "$work/plain")"
if [ -z "$plain" ]; then
    echo "SKIP: the build program did not report — it may not have run here"
    exit 0
fi

if [ "$plain" = "$inherited" ]; then
    echo "  ok  a project that declares nothing gets the PATH mcpp was started with"
else
    echo "FAIL: a project that declared no environment had its PATH changed"
    diff <(printf '%s\n' "$inherited" | tr ':' '\n') \
         <(printf '%s\n' "$plain"     | tr ':' '\n') | head -6 | sed 's/^/        /'
    exit 1
fi

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

if [ "$declared" = "$inherited" ]; then
    echo "FAIL: the declaration changed nothing — the environment is not in front"
    echo "        PATH: $(printf '%s' "$declared" | cut -c1-100)…"
    exit 1
fi

first="${declared%%:*}"
case "$first" in
  */subos/*/bin) echo "  ok  the declared environment is first: $first" ;;
  *) echo "FAIL: something other than a subos was prepended"
     echo "        got: $first"
     exit 1 ;;
esac

# ⭐ AND THE INHERITED VALUE IS STILL THERE, WHOLE, BEHIND IT. Prefixing means
# the rest is untouched; a test that only checked the first entry would pass on
# a PATH that had thrown everything else away, which would break every build
# program that calls `git`, `python3` or a shell.
if [ "${declared#*:}" = "$inherited" ]; then
    echo "  ok  and the inherited PATH follows it, unchanged"
else
    echo "FAIL: the inherited PATH was not preserved behind the prefix"
    diff <(printf '%s\n' "$inherited"      | tr ':' '\n') \
         <(printf '%s\n' "${declared#*:}"  | tr ':' '\n') | head -6 | sed 's/^/        /'
    exit 1
fi

echo "OK: the declaration puts an environment in front, and only the declaration does"
