#!/usr/bin/env bash
# requires: import-std-libcxx
# 40_llvm_bmi_cache.sh — Clang BMI cache reuse for dependency packages.
set -e

OS="$(uname -s)"
# libc++ std.cppm is only available on Linux/macOS — on Windows there is no
# libc++ module distribution. Exit gracefully; the import-std-libcxx capability
# check in run_all.sh already gates this, but guard here too for direct runs.
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]]; then
    echo "SKIP: libc++ std.cppm not available on Windows"
    exit 0
fi

source "$(dirname "$0")/_llvm_env.sh"

if [[ ! -x "$LLVM_ROOT/bin/clang++" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} is not installed"
    exit 0
fi
if [[ ! -f "$LLVM_ROOT/share/libc++/v1/std.cppm" ]]; then
    echo "SKIP: xlings llvm@${LLVM_VERSION} has no libc++ std.cppm"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

# mcpplibs packages live in a separate registry namespace; inherit it so the
# index lookup for mcpplibs.cmdline succeeds in the isolated MCPP_HOME.
USER_MCPP="${HOME}/.mcpp"
if [[ -d "$USER_MCPP/registry/data/mcpplibs" ]]; then
    mkdir -p "$MCPP_HOME/registry/data"
    [[ -e "$MCPP_HOME/registry/data/mcpplibs" ]] \
        || ln -sf "$USER_MCPP/registry/data/mcpplibs" "$MCPP_HOME/registry/data/mcpplibs" 2>/dev/null \
        || cp -r "$USER_MCPP/registry/data/mcpplibs" "$MCPP_HOME/registry/data/mcpplibs"
fi

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<EOF
[package]
name    = "llvm_cache"
version = "0.1.0"
[toolchain]
linux = "llvm@${LLVM_VERSION}"
[dependencies]
"mcpplibs.cmdline" = "0.0.1"
EOF

cat > src/main.cpp <<'EOF'
import std;
import mcpplibs.cmdline;
int main() {
    std::println("cache test ok");
    return 0;
}
EOF

# First build — populates the cache. Deliberately NOT `--no-cache`: that is now
# an alias for `--cache=off`, which means neither read NOR write, so a build that
# used it would leave nothing for the second build to reuse. MCPP_HOME is fresh
# here, so this build is already cold.
out1=$("$MCPP" build 2>&1)
echo "$out1" | grep -q "Compiling.*mcpplibs.cmdline" || {
    echo "FAIL: mcpplibs.cmdline not compiled in the first (cold) build: $out1"
    exit 1
}

# Second build, clean target dir, cache kept — the dependency must be reused.
rm -rf target
out2=$("$MCPP" build 2>&1)
echo "$out2" | grep -q "Cached.*mcpplibs.cmdline" || {
    echo "FAIL: mcpplibs.cmdline not cached on second build: $out2"
    exit 1
}

# ...and reuse must mean "not recompiled". The status line alone used to be
# printed while ninja rebuilt every unit behind it, so assert on the graph.
NINJA="$(find target -name build.ninja | head -1)"
[[ -n "$NINJA" ]] || { echo "FAIL: no build.ninja"; exit 1; }
if grep -qE ': (cxx_module|cxx_object|cxx_scan) .*mcpplibs' "$NINJA"; then
    echo "FAIL: cached dependency still has compile edges"
    grep -nE ': (cxx_module|cxx_object|cxx_scan) .*mcpplibs' "$NINJA" | head
    exit 1
fi

# Clang + module partitions: mcpplibs.cmdline has a `:options` partition, which a
# consumer never imports directly. Its stage edge must be sequenced before the
# consumer's compile, or clang fails with `failed to find module file for module
# 'mcpplibs.cmdline:options'` — a race Linux won and macOS lost.
grep -q '_mcpp_staged_cache' "$NINJA" || {
    echo "FAIL: staged artifacts are not sequenced before compilation"
    exit 1
}

# And the whole thing must actually link and run.
out3=$("$MCPP" run 2>&1) || { echo "FAIL: run: $out3"; exit 1; }
echo "$out3" | grep -q 'cache test ok' || { echo "FAIL: run output: $out3"; exit 1; }

echo "OK"
