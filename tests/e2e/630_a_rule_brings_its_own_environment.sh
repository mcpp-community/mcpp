#!/usr/bin/env bash
# requires: gcc
# A RULE PACKAGE DECLARES THE PAYLOADS ITS RULE NEEDS, AND THE PROJECT WRITES
# ONE EDGE.
#
# This is the arrangement the two preceding tests exist to make safe. A project
# that wants a device island used to write the rule edge AND repeat the rule's
# own package list with exact pins -- a copy that goes stale silently, because
# the rule moves and the projects do not.
#
# TWO GATES, AND BOTH MUST OPEN. The feature says whether the rule is wanted;
# the `cfg(accelerator = ...)` selector says which builds actually need the
# device toolkit. Both legs are here: without the second, an implementation
# that ignores the selector and downloads the toolkit for every build passes --
# and that build is the cheapest one, the one CI runs, and the one a vendor
# toolkit is gigabytes too expensive for.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

TOOL=zoxide

mkdir -p dep/src
cat > dep/src/lib.cpp <<'EOF'
int dep_touch() { return 1; }
EOF
cat > dep/build.mcpp <<EOF
#include <cstdio>
#include <string>
import mcpp;
int main() {
    const char* d = mcpp::xpkg_dir("xim", "$TOOL");
    std::string out = std::string(mcpp::manifest_dir()) + "/answered.txt";
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (f == nullptr) return 3;
    std::fprintf(f, "%s\n", d == nullptr ? "" : d);
    std::fclose(f);
    return 0;
}
EOF
# THE SHAPE mcpp:plugins USES: the payload is named by the rule, under the
# feature that selects the rule and the accelerator the rule serves.
cat > dep/mcpp.toml <<EOF
[package]
name    = "ruleowner"
version = "0.1.0"
[modules]
sources = ["src/**/*.cpp"]
[features]
default   = []
rules-toy = []
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-toy]
"xim:$TOOL" = ">=0.9.9"
[targets.ruleowner]
kind = "lib"
EOF

mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
# ONE EDGE, AND NOTHING ELSE. No [xlings.workspace] anywhere in the project.
cat > app/mcpp.toml <<'EOF'
[package]
name         = "consumer"
version      = "0.1.0"
accelerators = ["cuda"]
[dependencies]
ruleowner = { path = "../dep", features = ["rules-toy"] }
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
EOF

export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
STORE="$MCPP_HOME/registry/data/xpkgs/xim-x-$TOOL"

cd app

# ── leg 1: no accelerator, so neither gate opens ────────────────────────────
"$MCPP" build >cpu.log 2>&1 || { echo "FAIL: the CPU-only build did not build"; tail -15 cpu.log; exit 1; }
[ -d "$STORE" ] && {
    echo "FAIL: a build that named no accelerator downloaded the device payload"
    ls "$STORE"
    exit 1
}
echo "no accelerator: nothing installed"

# ── leg 2: the accelerator is named, so both gates open ─────────────────────
"$MCPP" build --accel "cuda12.9+{sm_89}" >dev.log 2>&1 || {
    echo "FAIL: the device build did not build"
    tail -15 dev.log
    exit 1
}
[ -d "$STORE" ] || {
    echo "FAIL: the rule's payload was never installed"
    tail -15 dev.log
    exit 1
}
answered="$TMP/dep/answered.txt"
[ -s "$answered" ] || { echo "FAIL: the rule's build program recorded nothing"; exit 1; }
echo "the rule's build program saw: $(cat "$answered")"
grep -q "xim-x-$TOOL" "$answered" || {
    echo "FAIL: the rule cannot find the payload it declared: $(cat "$answered")"
    exit 1
}

echo "PASS: the rule brings its own environment, and only when both gates open"
