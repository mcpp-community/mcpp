#!/usr/bin/env bash
# requires: llvm elf unix-shell
# 901 -- a managed LLVM's private library path reaches the toolchain's own
# programs and nothing else ninja starts (D29, design 2026-10-10 §13).
#
# It used to be in ninja's environment, so every `/bin/sh -c` ninja started
# inherited it. A shell linked against libc++ -- Android's bionic `sh` --
# then loaded the LLVM's glibc-built libc++ and died before running anything
# (`CANNOT LINK EXECUTABLE "/bin/sh"`, termux-docker aarch64). On a glibc
# host the shell survives, which is why the assertion reads the variable an
# action sees rather than waiting for a shell to fail.
#
#   L1  a check action run by ninja sees no LLVM directory in LD_LIBRARY_PATH;
#   L2  the build with `import std` still links and runs (the tools have it).
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/p/src" && cd "$TMP/p"
cat > mcpp.toml <<'TOML'
[package]
name = "lw"
version = "0.1.0"
[toolchain]
default = "llvm@23.1.3"
[targets.lw]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'import std;\nint main() { std::println("ran"); }\n' > src/main.cpp
cat > build.mcpp <<'CPP'
import std;
import mcpp;
int main() {
    const auto out = (std::filesystem::path(mcpp::out_dir()) / "ldpath.txt").generic_string();
    mcpp::action a;
    a.id = "ldpath";
    a.role = "check";
    a.arg("/bin/sh").arg("-c").arg(("echo \"LDP=[$LD_LIBRARY_PATH]\" > " + out).c_str())
     .output(out.c_str()).submit();
    return 0;
}
CPP

"$MCPP" build > build.log 2>&1 || fail "the llvm build failed" build.log
seen=$(cat "$(find target -name ldpath.txt | head -1)" 2>/dev/null)
[ -n "$seen" ] || fail "the check action did not run" build.log
case "$seen" in
    *xim-x-llvm*) fail "the toolchain's library path reached a shell ninja started: $seen" build.log ;;
esac
echo "ok L1 ($seen)"
[ "$(find target -path '*/bin/lw' -type f -exec {} \;)" = ran ] || fail "the program did not run" build.log
echo "ok L2"

echo "PASS: 901_the_toolchain_library_path_stays_with_the_tools"
