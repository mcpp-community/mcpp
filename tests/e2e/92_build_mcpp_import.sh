#!/usr/bin/env bash
# 92_build_mcpp_import.sh — the bundled `import mcpp;` typed build library. A
# build.mcpp written modules-first (no #include, no `import std;`) calls the typed
# API (mcpp::cxxflag / define / generated / link_lib …), which emits the same
# `mcpp:` wire protocol the engine parses. mcpp compiles the bundled module on the
# fly and links it. See .agents/docs/2026-06-30-l3-build-mcpp-implementation-design.md.
#
# requires: gcc
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p app/src
cd app
cat > mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF

# Modules-first build program: import mcpp;, no headers, no import std.
cat > build.mcpp <<'EOF'
import mcpp;
int main() {
    mcpp::generated("src/gen.cpp");          // a generated source to link
    mcpp::cxxflag("-DVIA_MODULE=1");          // a compile flag
    mcpp::define("MODULE_DEFINE");            // cfg=  → -DMODULE_DEFINE
    mcpp::rerun_if_changed("build.mcpp");
    return 0;
}
EOF
# build.mcpp uses generated() to declare src/gen.cpp; write it (the program could
# also generate it — here we just commit it so the test stays focused on import).
cat > src/gen.cpp <<'EOF'
int gen_value() { return 99; }
EOF
cat > src/main.cpp <<'EOF'
#if !defined(VIA_MODULE) || !defined(MODULE_DEFINE)
#error "import mcpp typed directives did not reach the build"
#endif
int gen_value();
int main() { return gen_value() == 99 ? 0 : 1; }
EOF

"$MCPP" build > b.log 2>&1 || { cat b.log; echo "FAIL: import mcpp build errored"; exit 1; }
grep -q "build.mcpp" b.log || { cat b.log; echo "FAIL: build.mcpp not invoked"; exit 1; }
# The bundled module is compiled into target/.build-mcpp/.
[ -f target/.build-mcpp/mcpp.o ] || { echo "FAIL: bundled mcpp module not compiled"; exit 1; }
# The binary returns 0 only if both the module-emitted define AND the generated
# source took effect.
"$MCPP" run > r.log 2>&1 || { cat r.log; echo "FAIL: run non-zero (module directives/generated source missing)"; exit 1; }

echo "OK"
