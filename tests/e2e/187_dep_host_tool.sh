#!/usr/bin/env bash
# requires: gcc
# 187_dep_host_tool.sh — #355: a dependency's `kind = "bin"` target, built for
# the HOST and handed to the consumer's build.mcpp.
#
# Why this cannot be done any other way: build.mcpp runs inside prepare, before
# the BuildPlan exists and long before build.ninja is written, so a tool
# produced by the main graph arrives too late to be called. The tool is
# therefore built by a nested, host-targeted sub-build into a global store.
#
# Covered here:
#   1. end to end — the tool is built, `mcpp::dep_bin()` finds it, the source it
#      generates is compiled and linked
#   2. the cost gate — a tool target behind `required_features` is built because
#      the sub-build ACTIVATES those features (in a tool sub-build the target is
#      what was asked for, so its requirements are inputs, not a gate)
#   3. default-off — a consumer that does not ask gets nothing built
#   4. a bad tool name fails with the available targets listed
#   5. the override escape hatch skips the build entirely
#
# See .agents/docs/2026-08-05-issue355-dependency-host-tools-design.md.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Isolate the tool store so a stale entry from a previous run cannot make a
# broken build look green.
export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
# Keep the real registry (toolchains are expensive) but not its build cache.
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

# ── the tool package ────────────────────────────────────────────────────────
mkdir -p toolpkg/src
cat > toolpkg/mcpp.toml <<'EOF'
[package]
name    = "toolpkg"
version = "0.1.0"

[build]
sources = ["src/lib.cpp"]

[features.codegen]
sources = ["src/codegen.cpp"]

[targets.codegen]
kind              = "bin"
main              = "src/codegen.cpp"
required_features = ["codegen"]
EOF
cat > toolpkg/src/lib.cpp <<'EOF'
int toolpkg_lib() { return 1; }
EOF
# A minimal "code generator": writes a C++ source whose function returns 42.
cat > toolpkg/src/codegen.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return 42; }\n");
    std::fclose(f);
    return 0;
}
EOF

# ── the consumer ────────────────────────────────────────────────────────────
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
toolpkg = { path = "../toolpkg", tools = ["codegen"] }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
cat > app/build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("toolpkg", "codegen");
    if (!tool || !*tool) { std::fprintf(stderr, "no tool path\n"); return 1; }
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "tool failed\n"); return 1; }
    mcpp::generated(out.c_str());
}
EOF

cd app
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: build with a dep host tool failed"; exit 1; }
grep -q "host tool" b1.log || { cat b1.log; echo "FAIL: no tool-build announcement"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=42" ]] || { echo "FAIL: generated source not linked: $out"; exit 1; }

# The store keeps the tool, so a second build must not rebuild it.
rm -rf target
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: second build failed"; exit 1; }
grep -q "Building.*host tool" b2.log && {
    cat b2.log; echo "FAIL: the tool was rebuilt despite a valid store entry"; exit 1; }

# ── default-off: no `tools = [...]`, nothing gets built ─────────────────────
cd "$TMP"
mkdir -p plain/src
cat > plain/mcpp.toml <<'EOF'
[package]
name    = "plain"
version = "0.1.0"

[dependencies]
toolpkg = { path = "../toolpkg" }
EOF
cat > plain/src/main.cpp <<'EOF'
int main() {}
EOF
cd plain
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: plain consumer build failed"; exit 1; }
grep -q "host tool" b3.log && {
    cat b3.log; echo "FAIL: a tool was provisioned for a consumer that never asked"; exit 1; }

# ── a bad tool name names the alternatives ──────────────────────────────────
cd "$TMP"
sed 's/tools = \["codegen"\]/tools = ["nosuchtool"]/' app/mcpp.toml > app/mcpp.toml.new
mv app/mcpp.toml.new app/mcpp.toml
cd app && rm -rf target
if "$MCPP" build > b4.log 2>&1; then
    cat b4.log; echo "FAIL: an unknown tool name was accepted"; exit 1
fi
grep -q "nosuchtool" b4.log || { cat b4.log; echo "FAIL: error does not name the request"; exit 1; }
grep -q "codegen" b4.log || {
    cat b4.log; echo "FAIL: error does not list the available bin targets"; exit 1; }

# ── the override escape hatch ───────────────────────────────────────────────
cd "$TMP"
sed 's/tools = \["nosuchtool"\]/tools = ["codegen"]/' app/mcpp.toml > app/mcpp.toml.new
mv app/mcpp.toml.new app/mcpp.toml
cat > fake_codegen.sh <<'EOF'
#!/usr/bin/env bash
printf 'int generated_answer() { return 7; }\n' > "$1"
EOF
chmod +x fake_codegen.sh
cd app && rm -rf target
MCPP_TOOL_TOOLPKG_CODEGEN="$TMP/fake_codegen.sh" "$MCPP" build > b5.log 2>&1 \
    || { cat b5.log; echo "FAIL: build with a tool override failed"; exit 1; }
grep -q "override" b5.log || { cat b5.log; echo "FAIL: the override was not reported"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^ANSWER=' | tail -1)"
[[ "$out" == "ANSWER=7" ]] || { echo "FAIL: the override was not actually used: $out"; exit 1; }

# ── a NAMESPACED package is addressable by both spellings ──────────────────
# `toolpkg` above has no namespace, so its canonical and short names are the
# same string and only one env var is emitted — the two-spelling path is never
# exercised. A consumer may write either `myns.tp` or `tp`, exactly as
# mcpp::dep_dir() accepts both, and the tool lookup has to match.
cd "$TMP"
mkdir -p ns/src
cat > ns/mcpp.toml <<'EOF'
[package]
name    = "myns.tp"
version = "0.1.0"

[build]
sources = ["src/lib.cpp"]

[targets.gen]
kind = "bin"
main = "src/gen.cpp"
EOF
printf 'int tp_lib(){return 1;}\n' > ns/src/lib.cpp
cat > ns/src/gen.cpp <<'EOF'
#include <cstdio>
int main(int c, char** v) {
    if (c < 2) return 2;
    FILE* f = std::fopen(v[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int gv() { return 5; }\n");
    std::fclose(f);
    return 0;
}
EOF
mkdir -p nsapp/src
cat > nsapp/mcpp.toml <<'EOF'
[package]
name    = "nsapp"
version = "0.1.0"

[dependencies]
"myns.tp" = { path = "../ns", tools = ["gen"] }
EOF
printf '#include <cstdio>\nint gv();\nint main(){std::printf("G=%%d\\n",gv());}\n' > nsapp/src/main.cpp
cat > nsapp/build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    // BOTH spellings must resolve to the same tool.
    const char* full  = mcpp::dep_bin("myns.tp", "gen");
    const char* brief = mcpp::dep_bin("tp", "gen");
    if (!*full)  { std::fprintf(stderr, "canonical spelling did not resolve\n"); return 1; }
    if (!*brief) { std::fprintf(stderr, "short spelling did not resolve\n");     return 1; }
    std::string out = std::string(mcpp::out_dir()) + "/g.cpp";
    std::string cmd = std::string("\"") + brief + "\" \"" + out + "\"";
    if (std::system(cmd.c_str()) != 0) return 1;
    mcpp::generated(out.c_str());
}
EOF
cd nsapp
"$MCPP" build > b6.log 2>&1 || { cat b6.log; echo "FAIL: namespaced tool lookup failed"; exit 1; }
out="$("$MCPP" run 2>&1 | grep '^G=' | tail -1)"
[[ "$out" == "G=5" ]] || { echo "FAIL: namespaced tool did not generate: $out"; exit 1; }

echo "OK"
