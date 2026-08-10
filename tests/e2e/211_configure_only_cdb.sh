#!/usr/bin/env bash
# requires:
# 211_configure_only_cdb.sh — `mcpp build --configure-only` must publish the
# compile database before compiling source files. The generated database is
# intended for clangd, so a broken source is deliberately part of the test.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/devkit/include" "$TMP/devkit/src"
cat > "$TMP/devkit/mcpp.toml" <<'EOF'
[package]
name = "devkit"
version = "0.1.0"

[build]
include_dirs = ["include"]
EOF
echo '#define DEVKIT_MARKER 1' > "$TMP/devkit/include/devkit.hpp"

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"

cat > mcpp.toml <<'EOF'
[package]
name = "configure-only"
version = "0.1.0"
standard = "c++23"

[build]
flags = [{ glob = "tests/**/*.cpp", cxxflags = ["-DMCPP_CONFIGURE_ONLY_TEST_FLAG=1"] }]

[dev-dependencies]
devkit = { path = "../devkit" }
EOF

# This must never reach a compiler in configure-only mode. It still needs a
# CDB entry so clangd can report the actual syntax error interactively.
printf 'int main( { return 0; }\n' > src/main.cpp
cat > tests/test_smoke.cpp <<'EOF'
#include <devkit.hpp>
int test_entry() { return 0; }
EOF

out=$("$MCPP" build --configure-only 2>&1) || {
    echo "configure-only failed unexpectedly:"
    echo "$out"
    exit 1
}

[[ -f compile_commands.json ]] || {
    echo "configure-only did not publish compile_commands.json"
    echo "$out"
    exit 1
}

grep -q 'src[\/][\/]*main.cpp' compile_commands.json || {
    echo "CDB is missing the ordinary source TU"
    cat compile_commands.json
    exit 1
}
grep -q 'tests[\/][\/]*test_smoke.cpp' compile_commands.json || {
    echo "CDB is missing the test TU"
    cat compile_commands.json
    exit 1
}
if command -v python3 >/dev/null 2>&1; then
    python3 - compile_commands.json <<'PY'
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
normal = lambda p: p.replace("\\", "/").rstrip("/")
test = next(e for e in entries if normal(e["file"]).endswith("/tests/test_smoke.cpp"))
main = next(e for e in entries if normal(e["file"]).endswith("/src/main.cpp"))
args = test["arguments"]
# Windows 原生进程与 MSYS 可能用不同根路径表示同一临时目录，
# 因此从 CDB 的 directory 字段推导相邻 devkit 路径。
fixture = normal(test["directory"]).rsplit("/", 1)[0]
expected_include = f"{fixture}/devkit/include".casefold()
include_args = {
    normal(a[2:]).casefold()
    for a in args
    if a[:2].casefold() == "-i"
}
assert expected_include in include_args, args
assert any("MCPP_CONFIGURE_ONLY_TEST_FLAG=1" in a for a in args), args
assert not any("MCPP_CONFIGURE_ONLY_TEST_FLAG=1" in a for a in main["arguments"]), main
PY
else
    grep -q -- '-DMCPP_CONFIGURE_ONLY_TEST_FLAG=1' compile_commands.json || {
        echo "CDB is missing [build].flags for the test TU"
        cat compile_commands.json
        exit 1
    }
fi

if find target -type f \( -name '*.o' -o -name '*.obj' \) -print -quit 2>/dev/null | grep -q .; then
    echo "configure-only produced an object file"
    find target -type f \( -name '*.o' -o -name '*.obj' \)
    exit 1
fi
[[ ! -d target/bin ]] || { echo "configure-only produced target/bin"; exit 1; }
[[ ! -e target/.build_cache ]] || { echo "configure-only wrote target/.build_cache"; exit 1; }

# Configuring an ALREADY-BUILT project must not poison the build fast path.
# The backend writes build.ninja before it honors dryRun, and a configure plan's
# graph is not a normal build's graph: it carries the test targets, so its
# `default` line names the TEST binaries and the package's own target is not in
# it at all. The fast path re-runs a cached build.ninja after comparing mtimes
# against the SOURCES — it never looks at the graph — so a surviving cache entry
# makes the next plain `mcpp build` link the tests, skip the target, and print
# `Finished`. Do NOT delete the built binary before the second build: a missing
# output makes ninja fail in a way the fast path reads as a stale graph and
# falls back to a full prepare, which hides exactly the defect under test.
mkdir -p "$TMP/fastpath/src" "$TMP/fastpath/tests"
cat > "$TMP/fastpath/mcpp.toml" <<'EOF'
[package]
name = "fastpath"
version = "0.1.0"
EOF
printf 'int main() { return 0; }\n' > "$TMP/fastpath/src/main.cpp"
printf 'int main() { return 0; }\n' > "$TMP/fastpath/tests/smoke.cpp"
cd "$TMP/fastpath"
"$MCPP" build > build-1.log 2>&1 || { cat build-1.log; exit 1; }
# Windows links `fastpath.exe`; the suffix is a host constant, so derive the
# whole path from what the build actually produced instead of assuming either.
target_bin=$(find target -type f \( -path '*/bin/fastpath' -o -path '*/bin/fastpath.exe' \) \
             -print -quit)
[[ -n "$target_bin" ]] || {
    echo "baseline build produced no binary"; find target -type f -path '*/bin/*'; exit 1; }
bindir=$(dirname "$target_bin")
exe_suffix=""
[[ "$target_bin" == *.exe ]] && exe_suffix=".exe"
test_bin="$bindir/smoke$exe_suffix"

"$MCPP" build --configure-only > configure-fastpath.log 2>&1 || {
    cat configure-fastpath.log; exit 1; }
[[ ! -e "$test_bin" ]] || { echo "configure-only linked a test binary"; exit 1; }

"$MCPP" build > build-2.log 2>&1 || { cat build-2.log; exit 1; }
[[ ! -e "$test_bin" ]] || {
    echo "build after configure-only replayed the configure graph and linked the tests"
    cat build-2.log
    ls -la "$bindir"
    exit 1
}
[[ -f "$target_bin" ]] || {
    echo "build after configure-only lost the package target"
    cat build-2.log
    exit 1
}

# A virtual workspace is configured member-by-member, with each member's CDB
# scoped to its own package. `-p` must select the same scope as normal build.
mkdir -p "$TMP/ws/a/src" "$TMP/ws/a/tests" "$TMP/ws/b/src" "$TMP/ws/b/tests"
cat > "$TMP/ws/mcpp.toml" <<'EOF'
[workspace]
members = ["a", "b"]
EOF
for member in a b; do
    cat > "$TMP/ws/$member/mcpp.toml" <<EOF
[package]
name = "$member"
version = "0.1.0"
standard = "c++23"
EOF
    printf 'int main() { return 0; }\n' > "$TMP/ws/$member/src/main.cpp"
    printf 'int test_entry() { return 0; }\n' > "$TMP/ws/$member/tests/main.cpp"
done
cd "$TMP/ws"
"$MCPP" build --configure-only > configure-workspace.log 2>&1 || {
    cat configure-workspace.log
    exit 1
}
for member in a b; do
    cdb="$member/compile_commands.json"
    [[ -s "$cdb" ]] || { echo "missing $cdb"; exit 1; }
    grep -q "src[\\/][\\/]*main.cpp" "$cdb" || { cat "$cdb"; exit 1; }
    grep -q "tests[\\/][\\/]*main.cpp" "$cdb" || { cat "$cdb"; exit 1; }
done
rm -f a/compile_commands.json
"$MCPP" build --configure-only -p a > configure-member.log 2>&1 || {
    cat configure-member.log
    exit 1
}
grep -q 'src[\/][\/]*main.cpp' a/compile_commands.json || { cat a/compile_commands.json; exit 1; }
if grep -q 'b[\/][\/]*src[\/][\/]*main.cpp' a/compile_commands.json; then
    echo "-p a leaked member b into the CDB"
    cat a/compile_commands.json
    exit 1
fi

# A failed replacement must be non-fatal for a normal build, but fatal for
# configure-only; the pre-existing destination must remain untouched.
mkdir -p "$TMP/publish-policy/src" "$TMP/publish-policy/compile_commands.json"
cat > "$TMP/publish-policy/mcpp.toml" <<'EOF'
[package]
name = "publish-policy"
version = "0.1.0"
standard = "c++20"
EOF
printf 'int main() { return 0; }\n' > "$TMP/publish-policy/src/main.cpp"
echo keep > "$TMP/publish-policy/compile_commands.json/last-known-good"
cd "$TMP/publish-policy"
normal_out=$("$MCPP" build 2>&1) || {
    echo "normal build failed on optional CDB publication:"
    echo "$normal_out"
    exit 1
}
[[ "$normal_out" == *"compile_commands.json was not updated"* ]] || {
    echo "normal build did not warn about CDB publication: $normal_out"
    exit 1
}
[[ -f compile_commands.json/last-known-good ]] || {
    echo "normal build removed the prior CDB destination"
    exit 1
}
configure_rc=0
configure_out=$("$MCPP" build --configure-only 2>&1) || configure_rc=$?
[[ $configure_rc -ne 0 ]] || {
    echo "configure-only accepted failed CDB publication: $configure_out"
    exit 1
}
[[ -f compile_commands.json/last-known-good ]] || {
    echo "configure-only removed the prior CDB destination"
    exit 1
}

echo "OK"
