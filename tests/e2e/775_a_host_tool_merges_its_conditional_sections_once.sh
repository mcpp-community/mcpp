#!/usr/bin/env bash
# 775 -- a dependency compiled as a host tool receives each entry of its
# matching `[target.<selector>.build]` sections once (#690, design record F12).
#
# The resolver merges a dependency's conditional sections for the consumer's
# target. A host-tool sub-build used to receive that merged manifest and merge
# it again for the host, so an entry of a section matching both reached the
# tool twice. The section here adds `-include once.h`, and `once.h` defines a
# variable without an include guard: included twice, the tool does not compile.
# The package builds on its own, which is the control.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

mkdir -p "$TMP/toolpkg/src" "$TMP/toolpkg/inc" "$TMP/app/src"
echo 'int once_counter = 1;' > "$TMP/toolpkg/inc/once.h"
cat > "$TMP/toolpkg/mcpp.toml" <<'EOF'
[package]
name    = "toolpkg"
version = "0.1.0"

[targets.gen]
kind = "bin"
main = "src/gen.cpp"

[build]
include_dirs = ["inc"]

[target.'cfg(not(os = "none"))'.build]
cxxflags = ["-include once.h"]
EOF
cat > "$TMP/toolpkg/src/gen.cpp" <<'EOF'
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return %d; }\n", 40 + once_counter);
    std::fclose(f);
    return 0;
}
EOF
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
toolpkg = { path = "../toolpkg", tools = ["gen"] }
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
cat > "$TMP/app/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "gen");
    if (!tool || !*tool) return 1;
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return 1;
    mcpp::generated(out.c_str());
}
EOF

( cd "$TMP/toolpkg" && "$MCPP" build > "$TMP/direct.log" 2>&1 ) \
    || fail "control: the tool package does not build on its own" "$TMP/direct.log"
cd "$TMP/app"
"$MCPP" build > build.log 2>&1 || fail "the host tool received a conditional entry twice" build.log
"$MCPP" run > run.log 2>&1 || fail "the program did not run" run.log
grep -q '^ANSWER=41' run.log || fail "wrong answer" run.log
echo "PASS: 775_a_host_tool_merges_its_conditional_sections_once"
