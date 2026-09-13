#!/usr/bin/env bash
# requires: llvm
# 663 -- a package supplies the C++ standard library while the C library stays
# the payload's (#630, item 4). `llvm.libcxx` carries libc++ and libc++abi as
# source with a std module; the engine reports the C++ layer as the graph's,
# withholds the payload's libc++ headers, links with -nostdlib++, and the
# program runs with no libc++ shared object in its closure. This is the
# combination the iOS rows need, measured here on Linux over glibc, which is
# the one host the shards have. The negative direction: the same program
# without the declaration keeps the payload's headers and its command lines.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
"$MCPP" new app > /dev/null
cd app
cat > src/main.cpp <<'CPP'
import std;
int main() {
    std::unordered_map<std::string, int> m;
    m["one"] = 1;
    std::atomic<int> a{2};
    a.notify_all();
    std::print("{}-{}-3\n", m["one"], a.load());
}
CPP
cat >> mcpp.toml <<'TOML'

[toolchain]
default = "llvm@22.1.8"
TOML

# The negative direction first, so that the baseline is read before the
# package changes anything: the payload's headers and its own runtime.
"$MCPP" build > base.log 2>&1 || fail "the baseline build failed" base.log
# The report prints a layer only when a package answers for it, so the
# baseline is read from the command lines rather than from a line that is
# absent by design.
grep -q 'c++-abi.*graph)' base.log && fail "the baseline report names a graph C++ layer" base.log
base_ninja=$(ls target/*/*/build.ninja | head -1)
grep -q -- '-isystem.*include/c++/v1' "$base_ninja" \
    || fail "the baseline compile line carries no payload libc++ -isystem" "$base_ninja"
# On this host the payload's contract is self-contained, so the link line
# names the payload's own archives; that, not `-nostdlib++`, is the mark of
# the payload's runtime.
grep -q -- 'xim-x-llvm[^ ]*libc++\.a' "$base_ninja" \
    || fail "the baseline link line does not name the payload's libc++.a" "$base_ninja"
echo "ok: without the package the payload's libc++ is used"

cat >> mcpp.toml <<'TOML'

[dependencies]
llvm.libcxx = { git = "https://github.com/mcpplibs/libcxx.git", tag = "22.1.8.1" }
TOML
rm -rf target
"$MCPP" build > build.log 2>&1 || fail "the build over llvm.libcxx failed" build.log
grep -E 'c\+\+-abi +libc\+\+ +\(libcxx@22\.1\.8\.1, graph\)' build.log \
    || fail "the report does not name llvm.libcxx as the C++ layer (graph)" build.log
ninja=$(ls target/*/*/build.ninja | head -1)
grep -q -- '-isystem[^ ]*xim-x-llvm[^ ]*include/c++/v1' "$ninja" \
    && fail "the payload's libc++ headers are still on a compile line" "$ninja"
grep -q -- '-nostdinc++' "$ninja" || fail "the compile lines carry no -nostdinc++" "$ninja"
grep -q -- '-nostdlib++' "$ninja" || fail "the link line carries no -nostdlib++" "$ninja"
grep -q -- 'xim-x-llvm[^ ]*libc++\(abi\)\?\.a' "$ninja" \
    && fail "the link line still names the payload's libc++ archives" "$ninja"
# The package's std module was precompiled against the payload's C library,
# not against whatever the driver found on the machine: the recorded command
# names the glibc payload. Before this assertion the command carried only
# `--no-default-config -nostdinc++` and the runner's /usr/include stood in,
# a host dependency no report showed.
stdjson=$(grep -l 'llvm.libcxx\|libcxx' "$MCPP_HOME"/build-cache/v1/std/*/std-module.json 2>/dev/null | xargs -r ls -t | head -1)
[ -n "$stdjson" ] || fail "no std-module.json records the package's module under $MCPP_HOME/build-cache/v1/std"
grep -q 'xim-x-glibc' "$stdjson" \
    || fail "the package std module's command does not name the payload's glibc" "$stdjson"
bin=$(ls target/*/*/bin/app | head -1)
ldd "$bin" | grep -q 'libc++' && fail "the artefact still links a libc++ shared object" <(ldd "$bin")
out=$("$bin") || fail "the program exited non-zero"
[ "$out" = "1-2-3" ] || fail "expected 1-2-3, got: $out"
echo "ok: llvm.libcxx supplies the C++ layer over the payload's C library, and the program runs"
