#!/usr/bin/env bash
# requires:
# 202_configure_only_cdb.sh — `mcpp build --configure-only` must publish the
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

out=$($MCPP build --configure-only 2>&1) || {
    echo "configure-only failed unexpectedly:"
    echo "$out"
    exit 1
}

[[ -f compile_commands.json ]] || {
    echo "configure-only did not publish compile_commands.json"
    echo "$out"
    exit 1
}

grep -q 'src/main.cpp' compile_commands.json || {
    echo "CDB is missing the ordinary source TU"
    cat compile_commands.json
    exit 1
}
grep -q 'tests/test_smoke.cpp' compile_commands.json || {
    echo "CDB is missing the test TU"
    cat compile_commands.json
    exit 1
}
if command -v python3 >/dev/null 2>&1; then
    python3 - compile_commands.json "$TMP/devkit/include" <<'PY'
import json, os, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
normal = lambda p: p.replace("\\", "/")
test = next(e for e in entries if normal(e["file"]).endswith("/tests/test_smoke.cpp"))
main = next(e for e in entries if normal(e["file"]).endswith("/src/main.cpp"))
args = test["arguments"]
assert any(os.path.normpath(sys.argv[2]) in os.path.normpath(a) for a in args), args
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
    grep -q "src/main.cpp" "$cdb" || { cat "$cdb"; exit 1; }
    grep -q "tests/main.cpp" "$cdb" || { cat "$cdb"; exit 1; }
done
rm -f a/compile_commands.json
"$MCPP" build --configure-only -p a > configure-member.log 2>&1 || {
    cat configure-member.log
    exit 1
}
grep -q 'src/main.cpp' a/compile_commands.json || { cat a/compile_commands.json; exit 1; }
if grep -q 'b/src/main.cpp' a/compile_commands.json; then
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
