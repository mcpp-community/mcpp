#!/usr/bin/env bash
# requires: gcc
# 997 -- a host tool has one store key whatever the command and the member
# (mcpp#874). The key's dependency closure was read from the graph the build
# had walked: empty where only a build dependency's `tools` reached the tool
# package (`mcpp test --workspace`), complete where it was a member being
# built (`mcpp build`), and holding the dev-dependencies of a member whose
# tests were built. The tool's path changed with the key, every action that
# ran it ran again, and its outputs rebuilt every importer. The closure is now
# the tool package's own manifests, as its build resolves them.
#
# Criteria:
#   A. `mcpp test --workspace`, then `mcpp build`: the tool has one store entry,
#      and it names `lib` (the tool's path dependency) and not `testing`
#      (`lib`'s dev-dependency).
#   B. A second `mcpp test --workspace` builds no host tool.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
[ -d "$HOME/.mcpp/registry" ] && ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p ws/testing/src ws/lib/src ws/lib/tests ws/toolpkg/src ws/app/src ws/app/tests
cd ws
cat > mcpp.toml <<'EOF'
[workspace]
members = ["testing", "lib", "toolpkg", "app"]
EOF
cat > testing/mcpp.toml <<'EOF'
[package]
name    = "testing"
version = "0.1.0"

[targets.testing]
kind = "lib"
EOF
echo 'int testing_ok() { return 0; }' > testing/src/testing.cpp
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "lib"
version = "0.1.0"

[targets.lib]
kind = "lib"

[dev-dependencies]
testing = { path = "../testing" }
EOF
echo 'int lib_value() { return 41; }' > lib/src/lib.cpp
cat > lib/tests/smoke.cpp <<'EOF'
int testing_ok();
int lib_value();
int main() { return testing_ok() + (lib_value() == 41 ? 0 : 1); }
EOF
cat > toolpkg/mcpp.toml <<'EOF'
[package]
name    = "toolpkg"
version = "0.1.0"

[dependencies]
lib = { path = "../lib" }

[targets.gen]
kind = "bin"
main = "src/gen.cpp"
EOF
cat > toolpkg/src/gen.cpp <<'EOF'
#include <cstdio>
int lib_value();
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return %d; }\n", lib_value() + 1);
    return std::fclose(f);
}
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[build-dependencies]
toolpkg = { path = "../toolpkg", tools = ["gen"] }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
cat > app/tests/answer.cpp <<'EOF'
int generated_answer();
int main() { return generated_answer() == 42 ? 0 : 1; }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    const std::string cmd = std::string("\"") + mcpp::dep_bin("toolpkg", "gen") + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return 1;
    mcpp::generated(out.c_str());
}
EOF

# A
"$MCPP" test --workspace > t1.log 2>&1 || fail "the first test run failed" t1.log
"$MCPP" build > b1.log 2>&1 || fail "the build failed" b1.log
keys=$(find "$MCPP_HOME/build-cache/v1/tool" -name entry.json -path "*toolpkg@*" | wc -l)
entries=$(find "$MCPP_HOME/build-cache/v1/tool" -name entry.json -path "*toolpkg@*")
[ "$keys" -eq 1 ] || fail "the tool has $keys store entries, expected 1" $entries
grep -q '"lib@' $entries || fail "the key does not name the tool's dependency lib" $entries
grep -q '"testing@' $entries && fail "the key names lib's dev-dependency testing" $entries

# B
"$MCPP" test --workspace > t2.log 2>&1 || fail "the second test run failed" t2.log
grep -q "Building.*host tool" t2.log && fail "the second test run built a host tool again" t2.log

echo "OK"
