#!/usr/bin/env bash
# requires: gcc
# 193_provision_reexport.sh — #359: a library standing up a toolchain on its
# user's behalf.
#
# Before this, `tools = [...]` and `host-module = true` were recorded against
# the consumer of the edge that ASKED. A library could therefore build a tool
# but not hand it on, so its user had to declare every tool the library needed
# — for grpc that meant four dependency lines and knowing that gRPC codegen
# runs protobuf's protoc, which is the library's knowledge, not the user's.
#
# Covered here:
#   1. WITHOUT `reexport`, a library's tool stays with the library. That is the
#      supply-chain default: an arbitrary transitive dependency must not be
#      able to put entries in your build program's tool namespace.
#   2. WITH `reexport = true`, the consumer's build.mcpp sees the tool, the
#      re-exported rule module, and the re-exported package's directory —
#      while declaring ONE dependency.
#
# See .agents/docs/2026-08-06-provisions-and-build-inputs.md.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

# ── the tool package (what a library depends on, and the user never names) ──
mkdir -p toolpkg/src
cat > toolpkg/mcpp.toml <<'EOF'
[package]
name    = "toolpkg"
version = "0.1.0"

[build]
sources = ["src/lib.cpp"]

[targets.codegen]
kind = "bin"
main = "src/codegen.cpp"
EOF
printf 'int toolpkg_lib(){return 1;}\n' > toolpkg/src/lib.cpp
cat > toolpkg/src/codegen.cpp <<'EOF'
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return 42; }\n");
    std::fclose(f);
    return 0;
}
EOF

# ── the rule package: an importable build rule, distributed as a package ────
mkdir -p rulepkg/src
cat > rulepkg/mcpp.toml <<'EOF'
[package]
name    = "rulepkg"
version = "0.1.0"

[lib]
path = "src/rulepkg.cppm"
EOF
cat > rulepkg/src/rulepkg.cppm <<'EOF'
export module rulepkg;
import std;
import mcpp;
export namespace rulepkg {
// The rule knows which tool it needs. Its consumer does not have to.
bool generate() {
    const char* tool = mcpp::dep_bin("toolpkg", "codegen");
    if (!tool || !*tool) {
        std::println(std::cerr, "rulepkg: no codegen tool");
        return false;
    }
    // The tool package's own tree must be reachable too — that is where a real
    // rule finds its data files (protoc's well-known .proto files).
    if (std::string(mcpp::dep_dir("toolpkg")).empty()) {
        std::println(std::cerr, "rulepkg: no dep_dir for toolpkg");
        return false;
    }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return false;
    mcpp::generated(out.c_str());
    return true;
}
}
EOF

# ── the library: it owns the knowledge, and re-exports what its user needs ──
mkdir -p lib/src
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "mylib"
version = "0.1.0"

[build]
sources = ["src/mylib.cpp"]

[dependencies]
toolpkg = { path = "../toolpkg", tools = ["codegen"] }
rulepkg = { path = "../rulepkg", host-module = true }
EOF
printf 'int mylib_fn(){return 1;}\n' > lib/src/mylib.cpp

# ── 1. no reexport: the tool is built for the library, not handed on ────────
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mylib = { path = "../lib" }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdio>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "codegen");
    std::printf("SEES_TOOL=%d\n", (tool && *tool) ? 1 : 0);
    return 1;   // fail on purpose: the build must not proceed either way
}
EOF
cd app
if "$MCPP" build > b1.log 2>&1; then
    cat b1.log; echo "FAIL: build.mcpp returned 1 but the build succeeded"; exit 1
fi
if grep -q "SEES_TOOL=1" b1.log; then
    cat b1.log
    echo "FAIL: a library's tool leaked to its consumer without reexport"
    exit 1
fi
grep -q "SEES_TOOL=0" b1.log || {
    cat b1.log; echo "FAIL: build.mcpp did not run"; exit 1; }

# ── 2. reexport: ONE dependency line is enough ──────────────────────────────
cd "$TMP"
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "mylib"
version = "0.1.0"

[build]
sources = ["src/mylib.cpp"]

[dependencies]
toolpkg = { path = "../toolpkg", tools = ["codegen"], reexport = true }
rulepkg = { path = "../rulepkg", host-module = true, reexport = true }
EOF
# The consumer names neither the tool nor the rule's dependencies — only the
# rule's module, which is what it actually calls.
cat > app/build.mcpp <<'EOF'
import mcpp;
import rulepkg;
int main() { return rulepkg::generate() ? 0 : 1; }
EOF
cd app && rm -rf target
"$MCPP" build > b2.log 2>&1 || {
    cat b2.log; echo "FAIL: build with re-exported provisions failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=42" ]] || {
    echo "FAIL: the re-exported tool's output was not linked: $out"; exit 1; }

# The rule is BUILD-TIME only: it must not have been compiled into the binary.
if grep -q "rulepkg" <(nm -C "target"/*/*/bin/app 2>/dev/null || true); then
    echo "FAIL: the rule package was linked into the consumer's binary"; exit 1
fi

# ── 3. a feature ADDING a request to an already-declared dependency ────────
# The real shape: gRPC depends on protobuf always, and its `codegen` feature
# has to add `tools = ["protoc"], reexport = true` to that SAME edge. Moving
# the request to the unconditional entry is not an option — it would make every
# consumer build protoc — and dropping the feature's spec (try_emplace keeps
# the existing key) would silently lose the request.
cd "$TMP"
cat > lib/mcpp.toml <<'EOF'
[package]
name    = "mylib"
version = "0.1.0"

[build]
sources = ["src/mylib.cpp"]

# Unconditional: the library links against this package no matter what.
[dependencies]
toolpkg = { path = "../toolpkg" }
rulepkg = { path = "../rulepkg", host-module = true, reexport = true }

# The feature adds a REQUEST to the edge above, and nothing else.
[feature-deps.codegen]
toolpkg = { path = "../toolpkg", tools = ["codegen"], reexport = true }
EOF
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
mylib = { path = "../lib", features = ["codegen"] }
EOF
cd app && rm -rf target
"$MCPP" build > b3.log 2>&1 || {
    cat b3.log
    echo "FAIL: a feature could not add a tool request to an existing dependency"
    exit 1
}
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=42" ]] || { echo "FAIL: expected ANSWER=42, got '$out'"; exit 1; }

# … and without the feature, nothing is built.
cd "$TMP"
sed 's/, features = \["codegen"\]//' app/mcpp.toml > app/mcpp.toml.new
mv app/mcpp.toml.new app/mcpp.toml
cd app && rm -rf target
if "$MCPP" build > b4.log 2>&1; then
    cat b4.log
    echo "FAIL: the build succeeded without the codegen feature — the tool ran anyway"
    exit 1
fi
grep -q "host tool" b4.log && {
    cat b4.log; echo "FAIL: a tool was built for a consumer that did not enable the feature"; exit 1; }

echo "PASS: 193_provision_reexport"
