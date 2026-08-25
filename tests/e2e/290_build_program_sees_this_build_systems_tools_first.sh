#!/usr/bin/env bash
# requires: gcc unix-shell
# A build program looks up a tool and finds the one mcpp installed.
#
# ⚠️ IT USED TO FIND WHATEVER THE MACHINE HAD, AND `command -v` CANNOT TELL THE
# DIFFERENCE.
#
# mcpp installs its tools into its own sub-OS — 221 programs on the machine
# this was written on, cross-compilers and emulators among them — and until
# 2026.8.25.1 none of them were on the PATH a build program inherited.
# Measured with a build program that printed its own PATH:
#
#     /home/…/.xlings/data/xpkgs/xim-x-claude/2.1.222
#     /home/…/mcpp/.xlings/subos/_/bin
#     /home/…/.xlings/subos/current/bin
#     subos/bin in PATH: NO
#
# The concrete cost, from the same day: `command -v qemu-system-riscv64`
# returned an xlings shim that answers, when run,
#
#     [error] qemu-system-riscv64 is not installed in this subos (_)
#
# — found, reported present, unable to execute, while the real one sat in
# mcpp's own directory unreachable.
#
# ⭐ PREPENDED, NOT SUBSTITUTED, AND THIS FILE ASSERTS BOTH HALVES. A build
# program legitimately reaches for things mcpp does not ship, so the host's
# entries must survive behind mcpp's. A test that only checked the first entry
# would pass on a PATH that had thrown the rest away.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app/src"
cd "$work/app"

cat > mcpp.toml <<'TOML'
[package]
name    = "pathprobe"
version = "0.1.0"
TOML
printf 'int main() { return 0; }\n' > src/main.cpp

# ⚠️ A NON-ZERO EXIT, BECAUSE THAT IS WHAT MAKES THE OUTPUT VISIBLE. mcpp
# prints a build program's stdout only when it fails — a probe that succeeds
# says nothing, which is a property of the protocol and not of this test.
cat > build.mcpp <<'CPP'
import std;

int main() {
    const char* p = std::getenv("PATH");
    std::string_view path(p ? p : "");
    std::string_view first;
    for (auto part : std::views::split(path, ':')) {
        first = std::string_view(part);
        break;
    }
    std::println("PROBE_FIRST={}", first);
    std::println("PROBE_ENTRIES={}", std::ranges::count(path, ':') + 1);
    return 1;
}
CPP

out="$("$MCPP" build 2>&1 || true)"

first="$(printf '%s\n' "$out" | grep -oP 'PROBE_FIRST=\K.*' | head -1)"
entries="$(printf '%s\n' "$out" | grep -oP 'PROBE_ENTRIES=\K[0-9]+' | head -1)"

if [ -z "$first" ]; then
    echo "SKIP: the build program did not report — it may not have run here"
    printf '%s\n' "$out" | grep -iE 'error' | head -3
    exit 0
fi

case "$first" in
  */subos/default/bin)
    echo "  ok  the first PATH entry is this build system's own tools: $first" ;;
  *)
    echo "FAIL: the first PATH entry is not mcpp's tools directory"
    echo "        got: $first"
    exit 1 ;;
esac

# ⭐ AND THE HOST IS STILL BEHIND IT. One entry means the inherited PATH was
# replaced rather than extended, which would break every build program that
# calls `git`, `python3` or a shell.
if [ "${entries:-1}" -gt 1 ]; then
    echo "  ok  the inherited PATH survives behind it ($entries entries)"
else
    echo "FAIL: PATH was replaced, not prefixed — only $entries entry"
    exit 1
fi

echo "OK: a build program finds this build system's tools first, and the host's after"
