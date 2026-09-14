#!/usr/bin/env bash
# requires: gcc
# 689 -- `mcpp pack --features <LIST>` activates root-package features for the
# build passes the pack performs (#641, item 4). A host tool needed only for one
# distribution is declared under `[feature-deps.<f>]` with `tools = [...]`, and
# `pack` is the command that asks for it; the option did not exist, so the
# declaration had to be unconditional and every plain build compiled the tool.
#
# Three readings, each against the other two: the pack with the feature builds
# the tool and ships the program the tool generated; the pack without it builds
# no tool; a plain build without it builds no tool either.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# An isolated store, so that a tool built by an earlier run cannot answer.
export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p toolpkg/src app/src
cat > toolpkg/mcpp.toml <<'EOF'
[package]
name    = "toolpkg"
version = "0.1.0"

[targets.gen]
kind = "bin"
main = "src/gen.cpp"
EOF
cat > toolpkg/src/gen.cpp <<'EOF'
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return 41; }\n");
    std::fclose(f);
    return 0;
}
EOF

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[features]
installer = {}

[feature-deps.installer]
toolpkg = { path = "../toolpkg", tools = ["gen"] }
EOF
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
EOF
# The build program asks for the tool only when the feature is active, which
# is the use the issue describes: without the feature, `dep_bin` is not reached.
cat > app/build.mcpp <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    if (mcpp::has_feature("installer")) {
        const char* tool = mcpp::dep_bin("toolpkg", "gen");
        if (!tool || !*tool) { std::fprintf(stderr, "no tool path\n"); return 1; }
        std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
        if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "tool failed\n"); return 1; }
    } else {
        FILE* f = std::fopen(out.c_str(), "w");
        if (!f) return 1;
        std::fprintf(f, "int generated_answer() { return 0; }\n");
        std::fclose(f);
    }
    mcpp::generated(out.c_str());
}
EOF
cd app

packed_answer() {   # $1 = log; runs the program the last pack staged
    local exe
    exe=$(find target/dist -type f -name app -perm -u+x 2>/dev/null | head -1)
    [ -n "$exe" ] || fail "the pack staged no program" "$1"
    "$exe"
}
tool_entries() { find "$MCPP_HOME/build-cache" -path '*/tool/*' -name 'gen*' -type f 2>/dev/null | wc -l; }

# ── The pack without the feature: no tool, the fallback source ─────────────
"$MCPP" pack --format dir > p0.log 2>&1 || fail "pack without the feature failed" p0.log
[ "$(packed_answer p0.log)" = "ANSWER=0" ] || fail "pack without the feature: expected 0" p0.log
[ "$(tool_entries)" -eq 0 ] || fail "a pack without the feature built the host tool" p0.log
grep -q "host tool" p0.log && fail "a pack without the feature mentioned the host tool" p0.log
echo "ok: a pack without --features builds no host tool"

# ── The pack with the feature: the tool is built and its output shipped ────
rm -rf target
"$MCPP" pack --format dir --features installer > p1.log 2>&1 \
    || fail "mcpp pack --features installer failed" p1.log
grep -q "unknown option" p1.log && fail "pack did not accept --features" p1.log
grep -q "Building.*host tool" p1.log || fail "the pack with the feature did not build the tool" p1.log
[ "$(packed_answer p1.log)" = "ANSWER=41" ] || fail "pack with the feature: expected 41" p1.log
echo "ok: mcpp pack --features activates the feature-gated host tool"

# ── A plain build without the feature: no tool ───────────────────────────
rm -rf target "$MCPP_HOME/build-cache"
"$MCPP" build > b0.log 2>&1 || fail "the plain build failed" b0.log
[ "$(tool_entries)" -eq 0 ] || fail "a plain build without the feature built the host tool" b0.log
echo "ok: a plain build without the feature builds no host tool"

echo "PASS: mcpp pack --features reaches the build passes of the pack"
