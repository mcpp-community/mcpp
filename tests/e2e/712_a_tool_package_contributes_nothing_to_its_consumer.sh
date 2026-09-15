#!/usr/bin/env bash
# requires: gcc
# 712 -- a package whose declared targets are all programs contributes nothing
# to a consumer's graph; its tools come from the tool sub-build (#649 E6).
#
# An SDK provides a program behind a feature, and the program depends on the
# SDK. The tool sub-build resolves the program's package as its own root, so
# the program and the SDK it links are built there. The consumer's graph used
# to walk the program package's `[dependencies]` as well, which made the
# declaration a cycle of the consumer's graph ("dependency cycle through
# package 'fw' while computing its build-cache key", default cache only) and
# linked a tool's own library into the application.
#
# Readings: the SDK fixture builds with the default cache, the tool runs, and
# the build program reads its path under both spellings; a tool's own library
# is not linked into the application; a cycle among libraries is refused with
# the same message under both cache modes; a tool whose own build activates
# the feature that requests it is refused once, naming the repetition.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# fixture <dir> <tool dependency line>
fixture() {
    local W=$1
    mkdir -p "$W/fw/src" "$W/fw/tool/src" "$W/app/src" "$W/z/src"
    cat > "$W/fw/mcpp.toml" <<'EOF'
[package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[features]
installer = []

[feature-deps.installer]
spike.fw-installer = { path = "tool", tools = ["fw-installer"], reexport = true }
EOF
    echo 'int fw_answer() { return 42; }' > "$W/fw/src/fw.cpp"
    cat > "$W/fw/tool/mcpp.toml" <<EOF
[package]
name      = "fw-installer"
namespace = "spike"
version   = "0.1.0"

[targets.fw-installer]
kind = "bin"
main = "src/main.cpp"

[dependencies]
$2
EOF
    cat > "$W/z/mcpp.toml" <<'EOF'
[package]
name      = "z"
namespace = "spike"
version   = "0.1.0"

[targets.z]
kind = "lib"

[build]
sources = ["src/*.cpp"]
EOF
    echo 'int z_only_in_the_tool() { return 7; }' > "$W/z/src/z.cpp"
    cat > "$W/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = "../fw" }

[features]
windows-installer = ["spike.fw/installer"]
EOF
    cat > "$W/app/src/main.cpp" <<'EOF'
int fw_answer();
int main() { return fw_answer() == 42 ? 0 : 1; }
EOF
    cat > "$W/app/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("fw-installer", "fw-installer");
    const char* b = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    mcpp::warning((std::string("DEPBIN short=[") + (a ? a : "") + "] qualified=["
                   + (b ? b : "") + "]").c_str());
    return 0;
}
EOF
}

# ── 1. the SDK shape: the tool depends on the package declaring it ────────
fixture "$TMP/s1" 'spike.fw = { path = ".." }'
cat > "$TMP/s1/fw/tool/src/main.cpp" <<'EOF'
#include <cstdio>
int fw_answer();
int main() { std::printf("fw-installer ran %d\n", fw_answer()); return 0; }
EOF
cd "$TMP/s1/app"
"$MCPP" build --features windows-installer > b1.log 2>&1 \
    || fail "a feature tool depending on its declaring package did not build" b1.log
tool=$(grep -o 'DEPBIN short=\[[^]]*' b1.log | sed 's/DEPBIN short=\[//')
[ -n "$tool" ] || fail "dep_bin(\"fw-installer\") read nothing" b1.log
grep -q 'qualified=\[[^]]' b1.log || fail "dep_bin(\"spike.fw-installer\") read nothing" b1.log
out=$("$tool") || fail "the tool did not run: $out"
[ "$out" = "fw-installer ran 42" ] || fail "the tool printed '$out'"
"$MCPP" run > r1.log 2>&1 || fail "the application does not run" r1.log

# ── 2. a tool's own library does not reach the application ──────────────
fixture "$TMP/s2" 'spike.z = { path = "../../z" }'
cat > "$TMP/s2/fw/tool/src/main.cpp" <<'EOF'
#include <cstdio>
int z_only_in_the_tool();
int main() { std::printf("tool %d\n", z_only_in_the_tool()); return 0; }
EOF
cd "$TMP/s2/app"
"$MCPP" build --features windows-installer > b2.log 2>&1 || fail "the tool-with-a-library build failed" b2.log
ninja_file=$(find target -name build.ninja | head -1)
grep '^build bin/app' "$ninja_file" | grep -q 'spike_z' \
    && fail "the tool's own library was linked into the application" "$ninja_file"
grep -q 'spike_fw-installer' "$ninja_file" \
    && fail "the tool package's sources were compiled in the consumer's build" "$ninja_file"

# ── 3. a cycle among libraries is refused under both cache modes ─────────
mkdir -p "$TMP/s3/a/src" "$TMP/s3/b/src" "$TMP/s3/app/src"
for p in a b; do
    q=$([ "$p" = a ] && echo b || echo a)
    cat > "$TMP/s3/$p/mcpp.toml" <<EOF
[package]
name      = "$p"
namespace = "spike"
version   = "0.1.0"

[targets.$p]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[dependencies]
spike.$q = { path = "../$q" }
EOF
    echo "int ${p}_answer() { return 1; }" > "$TMP/s3/$p/src/$p.cpp"
done
cat > "$TMP/s3/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.a = { path = "../a" }
EOF
echo 'int main() { return 0; }' > "$TMP/s3/app/src/main.cpp"
cd "$TMP/s3/app"
for mode in "" "--cache=local"; do
    if "$MCPP" build $mode > b3.log 2>&1; then
        fail "a cycle of packages was accepted (${mode:-default cache})" b3.log
    fi
    grep -q "dependency cycle: 'spike.a' -> 'spike.b' -> 'spike.a'" b3.log \
        || fail "the cycle refusal does not name its edges (${mode:-default cache})" b3.log
done

# ── 4. a tool whose own build requests it again is refused once ──────────
fixture "$TMP/s4" 'spike.fw = { path = "..", features = ["installer"] }'
echo 'int main() { return 0; }' > "$TMP/s4/fw/tool/src/main.cpp"
cd "$TMP/s4/app"
if "$MCPP" build --features windows-installer > b4.log 2>&1; then
    fail "a tool requesting itself through its own build was accepted" b4.log
fi
grep -q "is requested by its own build" b4.log || fail "the repetition is not named" b4.log
[ "$(grep -o 'building host tool' b4.log | wc -l | tr -d ' ')" -le 1 ] \
    || fail "the repetition was refused only after nested sub-builds" b4.log

echo "PASS: 712 a package of programs contributes nothing to its consumer's graph"
